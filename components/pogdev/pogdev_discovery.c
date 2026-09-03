#include "pogdev_discovery.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include <errno.h>
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
#define FALLBACK_AFTER_MS  30000

static pogdev_server_t s_server;
static bool s_have_server;
static SemaphoreHandle_t s_lock;

/* Resolve the configured protocol-v1 base URL without teaching the enrolment
 * code about a second transport. HTTPS is deliberately rejected here: the v1
 * device contract only defines HTTP on the trusted LAN, and silently treating
 * an https:// URL as clear text would be worse than remaining undiscovered. */
static bool fallback_server(const char *url, pogdev_server_t *out) {
  static const char prefix[] = "http://";
  if (url == NULL || strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
    return false;
  }

  const char *authority = url + sizeof(prefix) - 1;
  const char *end = authority + strcspn(authority, "/?#");
  if (authority == end || strchr(authority, '@') != NULL ||
      (*end != '\0' && !(end[0] == '/' && end[1] == '\0'))) {
    return false;
  }

  const char *colon = NULL;
  for (const char *p = authority; p < end; ++p) {
    if (*p == ':') {
      /* IPv6 literals are not supported by this firmware. */
      if (colon != NULL) {
        return false;
      }
      colon = p;
    }
  }
  const char *host_end = colon != NULL ? colon : end;
  size_t host_len = (size_t)(host_end - authority);
  if (host_len == 0 || host_len >= sizeof(out->host)) {
    return false;
  }

  uint16_t port = 8090;
  if (colon != NULL) {
    char port_text[6];
    size_t port_len = (size_t)(end - colon - 1);
    if (port_len == 0 || port_len >= sizeof(port_text)) {
      return false;
    }
    memcpy(port_text, colon + 1, port_len);
    port_text[port_len] = '\0';
    errno = 0;
    char *tail = NULL;
    long parsed = strtol(port_text, &tail, 10);
    if (errno != 0 || tail == NULL || *tail != '\0' || parsed < 1 ||
        parsed > 65535) {
      return false;
    }
    port = (uint16_t)parsed;
  }

  char host[sizeof(out->host)];
  memcpy(host, authority, host_len);
  host[host_len] = '\0';
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *addresses = NULL;
  if (getaddrinfo(host, NULL, &hints, &addresses) != 0 || addresses == NULL) {
    return false;
  }

  const struct sockaddr_in *resolved =
      (const struct sockaddr_in *)addresses->ai_addr;
  memset(out, 0, sizeof(*out));
  strlcpy(out->host, host, sizeof(out->host));
  out->addr.addr = resolved->sin_addr.s_addr;
  out->api_port = port;
  out->mqtt_port = 1883;
  out->tls = false;
  out->proto = 1;
  freeaddrinfo(addresses);
  return true;
}

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

/* Retient le meilleur résultat IPv4, pas simplement le premier.
 *
 * Un foyer peut encore annoncer un ancien POG Home via Tailscale ou un pont de
 * virtualisation. mDNS ne garantit aucun ordre : le premier résultat peut donc
 * être parfaitement valide sur le papier mais inaccessible depuis l'ESP32.
 * Le serveur présent sur le même sous-réseau que WIFI_STA_DEF gagne toujours ;
 * à défaut, le contrat LAN v1 en clair est préféré à un relais TLS. */
static bool take_result(const mdns_result_t *r, pogdev_server_t *out) {
  esp_netif_ip_info_t wifi = {0};
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  bool have_wifi = sta != NULL && esp_netif_get_ip_info(sta, &wifi) == ESP_OK &&
                   wifi.ip.addr != 0 && wifi.netmask.addr != 0;
  bool have_candidate = false;
  int best_score = -1;

  for (; r != NULL; r = r->next) {
    if (r->addr == NULL) {
      continue;
    }
    for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
      if (a->addr.type != ESP_IPADDR_TYPE_V4) {
        continue;
      }
      pogdev_server_t candidate;
      memset(&candidate, 0, sizeof(candidate));
      if (r->hostname != NULL) {
        strlcpy(candidate.host, r->hostname, sizeof(candidate.host));
      }
      candidate.addr = a->addr.u_addr.ip4;
      /* Le port du service EST le port de l'API : la découverte doit rendre
       * quelque chose d'interrogeable. Le port MQTT voyage dans le TXT. */
      candidate.api_port = txt_u16(r, "api", r->port);
      candidate.mqtt_port = txt_u16(r, "mqtt", 1883);
      candidate.tls = txt_flag(r, "tls");
      candidate.proto = (int)txt_u16(r, "proto", 1);

      int score = 0;
      if (have_wifi && (candidate.addr.addr & wifi.netmask.addr) ==
                           (wifi.ip.addr & wifi.netmask.addr)) {
        score += 100;
      }
      if (!candidate.tls) {
        score += 10;
      }
      if (candidate.proto == 1) {
        score += 5;
      }
      if (!have_candidate || score > best_score) {
        *out = candidate;
        best_score = score;
        have_candidate = true;
      }
    }
  }
  return have_candidate;
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

  const int64_t started_us = esp_timer_get_time();
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

    /* Account for the just-completed mDNS timeout so a silent network falls
     * back at roughly 30 seconds rather than waiting for another 10-second
     * polling interval. */
    if (!found && CONFIG_POGDEV_HOME_FALLBACK_URL[0] != '\0' &&
        (esp_timer_get_time() - started_us) / 1000 + QUERY_TIMEOUT_MS >=
            FALLBACK_AFTER_MS) {
      pogdev_server_t fallback;
      if (fallback_server(CONFIG_POGDEV_HOME_FALLBACK_URL, &fallback)) {
        found = true;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_server = fallback;
        s_have_server = true;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG,
                 "mDNS muet depuis 30 s, repli configuré : %s (" IPSTR
                 ") api=%u",
                 fallback.host, IP2STR(&fallback.addr), fallback.api_port);
      } else {
        ESP_LOGW(TAG, "URL de repli POG Home invalide ou non résolue : %s",
                 CONFIG_POGDEV_HOME_FALLBACK_URL);
      }
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
