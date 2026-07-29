#include "pogdev_discovery.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pogdev";

#define POGDEV_SERVICE "_poghome"
#define POGDEV_PROTO   "_tcp"

/* Une requête mDNS ponctuelle plutôt qu'une souscription : la découverte est un
 * événement rare, et une requête toutes les N secondes coûte moins qu'un
 * navigateur permanent — en RAM comme en trafic multicast. */
#define QUERY_TIMEOUT_MS  3000
#define QUERY_MAX_RESULTS 4

/* Une fois le serveur trouvé on ralentit fortement : il ne bouge pas. On
 * continue quand même de vérifier, parce qu'il peut changer d'adresse (DHCP,
 * machine remplacée) et que le firmware doit suivre sans qu'on le reflashe. */
#define RETRY_NOT_FOUND_MS 10000
#define RETRY_FOUND_MS     300000

static pogdev_server_t s_server;
static bool s_have_server;
static SemaphoreHandle_t s_lock;

static uint16_t txt_u16(const mdns_result_t *r, const char *key,
                        uint16_t fallback) {
  for (size_t i = 0; i < r->txt_count; i++) {
    if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0 && r->txt[i].value) {
      int v = atoi(r->txt[i].value);
      if (v > 0 && v <= 65535) {
        return (uint16_t)v;
      }
    }
  }
  return fallback;
}

static bool txt_flag(const mdns_result_t *r, const char *key) {
  for (size_t i = 0; i < r->txt_count; i++) {
    if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0 && r->txt[i].value) {
      return r->txt[i].value[0] == '1';
    }
  }
  return false;
}

/* Retient le premier résultat qui porte une adresse IPv4.
 *
 * On ignore les enregistrements sans A : sur un Mac, le même service est publié
 * sur plusieurs interfaces (Wi-Fi, ponts de virtualisation, Tailscale) et
 * certaines réponses arrivent sans adresse exploitable. Prendre la première
 * IPv4 vue est le comportement attendu ici — l'ESP32 n'a qu'un seul réseau. */
static bool take_result(const mdns_result_t *r, pogdev_server_t *out) {
  for (; r != NULL; r = r->next) {
    if (r->addr == NULL) {
      continue;
    }
    for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) {
        continue;
      }
      memset(out, 0, sizeof(*out));
      if (r->hostname != NULL) {
        strlcpy(out->host, r->hostname, sizeof(out->host));
      }
      out->addr = a->addr.u_addr.ip4;
      /* Le port du service EST le port de l'API : la découverte doit rendre
       * quelque chose d'interrogeable. Le port MQTT voyage dans le TXT. */
      out->api_port = r->port;
      out->api_port = txt_u16(r, "api", out->api_port);
      out->mqtt_port = txt_u16(r, "mqtt", 1883);
      out->tls = txt_flag(r, "tls");
      out->proto = (int)txt_u16(r, "proto", 1);
      return true;
    }
  }
  return false;
}

static void discovery_task(void *arg) {
  (void)arg;

  /* mdns_airplay_init() a déjà fait l'initialisation sur cette carte ; un
   * second appel renvoie ESP_ERR_INVALID_STATE, ce qui n'est pas une erreur
   * pour nous. Le faire quand même rend le composant utilisable tel quel sur un
   * firmware POG qui n'annonce rien (LEDTest, CYD). */
  esp_err_t err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "mdns_init a échoué : %s", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }

  for (;;) {
    mdns_result_t *results = NULL;
    err = mdns_query_ptr(POGDEV_SERVICE, POGDEV_PROTO, QUERY_TIMEOUT_MS,
                         QUERY_MAX_RESULTS, &results);

    bool found = false;
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "requête mDNS en échec : %s", esp_err_to_name(err));
    } else if (results == NULL) {
      ESP_LOGI(TAG,
               "aucun serveur pog Home sur " POGDEV_SERVICE "." POGDEV_PROTO);
    } else {
      pogdev_server_t found_server;
      if (take_result(results, &found_server)) {
        found = true;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_server = found_server;
        s_have_server = true;
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG,
                 "pog Home trouvé : %s (" IPSTR
                 ") api=%u mqtt=%u tls=%d proto=%d",
                 found_server.host, IP2STR(&found_server.addr),
                 found_server.api_port, found_server.mqtt_port,
                 (int)found_server.tls, found_server.proto);
      } else {
        ESP_LOGW(TAG,
                 "enregistrement trouvé mais sans adresse IPv4 exploitable");
      }
    }
    if (results != NULL) {
      mdns_query_results_free(results);
    }

    vTaskDelay(pdMS_TO_TICKS(found ? RETRY_FOUND_MS : RETRY_NOT_FOUND_MS));
  }
}

esp_err_t pogdev_discovery_start(void) {
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  /* 4 ko suffisent : la tâche n'appelle que mDNS et journalise. Elle vit en
   * priorité basse — la découverte ne doit jamais disputer du temps à la
   * chaîne audio, qui est temps réel. */
  BaseType_t ok =
      xTaskCreate(discovery_task, "pogdev_disc", 4096, NULL, 3, NULL);
  return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool pogdev_discovery_get(pogdev_server_t *out) {
  if (out == NULL || s_lock == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool have = s_have_server;
  if (have) {
    *out = s_server;
  }
  xSemaphoreGive(s_lock);
  return have;
}
