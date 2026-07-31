/* pogdev — phase 2 : s'annoncer, puis relever ses identifiants.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §3.
 *
 * L'appareil s'annonce, un humain clique dans pog Home, l'appareil récupère un
 * compte MQTT borné à ses propres sujets. Aucun secret partagé n'est compilé
 * ici : c'est ce qui permet d'enrôler une carte sans écran ni clavier.
 */
#include "pogdev_enrol.h"
#include "pogdev_discovery.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "pogdev";

#define NVS_NS            "pogdev"
#define NVS_KEY_CLAIM     "claim"
#define NVS_KEY_DEVICE_ID "dev_id"
#define NVS_KEY_MQTT_PW   "mqtt_pw"
#define NVS_KEY_MQTT_HOST "mqtt_host"
#define NVS_KEY_MQTT_PORT "mqtt_port"

/* Cadence de relève de pog-docs §3.3 : celui qui installe n'est pas forcément
 * celui qui clique, donc on reste attentif au début puis on s'espace, sans
 * jamais abandonner. */
#define POLL_FAST_MS         5000
#define POLL_FAST_UNTIL_MS   60000
#define POLL_MEDIUM_MS       30000
#define POLL_MEDIUM_UNTIL_MS 3600000
#define POLL_SLOW_MS         300000

#define HTTP_TIMEOUT_MS 8000
#define RESP_MAX        1024

static pogdev_creds_t s_creds;
static bool s_have_creds;
static SemaphoreHandle_t s_lock;
static char s_hw_id[48];
static char s_claim_secret[65];
static char s_device_name[65];

/* ---- identité ---- */

/* Le hw_id est l'ancre de toute la chaîne : instable, il crée un appareil
 * fantôme par redémarrage. On le tire de ESP_MAC_BASE et non de
 * ESP_MAC_WIFI_STA — sur les cartes sans radio locale (P4) la MAC WiFi est non
 * initialisée, et autant que la règle soit la même partout. */
static void build_hw_id(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BASE);
  snprintf(s_hw_id, sizeof(s_hw_id), "ESP-AIRPLAY-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- NVS ---- */

static esp_err_t nvs_get_str_into(nvs_handle_t h, const char *key, char *out,
                                  size_t cap) {
  size_t len = cap;
  return nvs_get_str(h, key, out, &len);
}

static bool load_state(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return false;
  }
  nvs_get_str_into(h, NVS_KEY_CLAIM, s_claim_secret, sizeof(s_claim_secret));

  pogdev_creds_t c = {0};
  bool ok = nvs_get_str_into(h, NVS_KEY_DEVICE_ID, c.device_id,
                             sizeof(c.device_id)) == ESP_OK &&
            nvs_get_str_into(h, NVS_KEY_MQTT_PW, c.mqtt_password,
                             sizeof(c.mqtt_password)) == ESP_OK &&
            nvs_get_str_into(h, NVS_KEY_MQTT_HOST, c.mqtt_host,
                             sizeof(c.mqtt_host)) == ESP_OK;
  if (ok) {
    uint16_t port = 1883;
    nvs_get_u16(h, NVS_KEY_MQTT_PORT, &port);
    c.mqtt_port = port;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_creds = c;
    s_have_creds = true;
    xSemaphoreGive(s_lock);
  }
  nvs_close(h);
  return ok;
}

/* Le secret est écrit AVANT la première annonce : c'est lui qui prouvera plus
 * tard que c'est bien nous qui relevons les identifiants, et le serveur le fige
 * dès la première annonce reçue. Le perdre entre-temps veut dire recommencer.
 */
static esp_err_t ensure_claim_secret(void) {
  if (s_claim_secret[0] != '\0') {
    return ESP_OK;
  }
  uint8_t raw[32];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) {
    snprintf(s_claim_secret + i * 2, 3, "%02x", raw[i]);
  }

  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_str(h, NVS_KEY_CLAIM, s_claim_secret);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

static esp_err_t store_creds(const pogdev_creds_t *c) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  if ((err = nvs_set_str(h, NVS_KEY_DEVICE_ID, c->device_id)) == ESP_OK &&
      (err = nvs_set_str(h, NVS_KEY_MQTT_PW, c->mqtt_password)) == ESP_OK &&
      (err = nvs_set_str(h, NVS_KEY_MQTT_HOST, c->mqtt_host)) == ESP_OK &&
      (err = nvs_set_u16(h, NVS_KEY_MQTT_PORT, c->mqtt_port)) == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

/* ---- HTTP ---- */

/* Un aller-retour, corps lu dans un tampon fourni. Renvoie le code HTTP, ou -1.
 */
static int http_call(const char *url, const char *method, const char *body,
                     char *resp, size_t resp_cap) {
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = HTTP_TIMEOUT_MS,
      .method =
          strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
  };
  esp_http_client_handle_t cli = esp_http_client_init(&cfg);
  if (cli == NULL) {
    return -1;
  }
  if (body != NULL) {
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, (int)strlen(body));
  }

  int status = -1;
  if (esp_http_client_open(cli, body != NULL ? (int)strlen(body) : 0) ==
      ESP_OK) {
    if (body != NULL) {
      esp_http_client_write(cli, body, (int)strlen(body));
    }
    esp_http_client_fetch_headers(cli);
    status = esp_http_client_get_status_code(cli);
    if (resp != NULL && resp_cap > 0) {
      int n = esp_http_client_read(cli, resp, (int)resp_cap - 1);
      resp[n > 0 ? n : 0] = '\0';
    }
  }
  esp_http_client_cleanup(cli);
  return status;
}

/* ---- protocole ---- */

static void announce(const pogdev_server_t *srv) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "hw_id", s_hw_id);
  cJSON_AddStringToObject(body, "model", POGDEV_MODEL);
  cJSON_AddStringToObject(body, "fw_version", pogdev_fw_version());
  cJSON_AddStringToObject(body, "proto_version", "1");
  cJSON_AddStringToObject(body, "name",
                          s_device_name[0] ? s_device_name : "Enceinte POG");
  cJSON_AddStringToObject(body, "claim_secret", s_claim_secret);
  char *json = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (json == NULL) {
    return;
  }

  char url[128];
  snprintf(url, sizeof(url), "http://" IPSTR ":%u/api/v1/pogdev/announce",
           IP2STR(&srv->addr), srv->api_port);

  int status = http_call(url, "POST", json, NULL, 0);
  cJSON_free(json);

  if (status == 202) {
    ESP_LOGI(TAG, "annoncé à pog Home, en attente d'adoption (%s)", s_hw_id);
  } else {
    ESP_LOGW(TAG, "annonce refusée (HTTP %d)", status);
  }
}

/* Renvoie true quand les identifiants ont été relevés et enregistrés. */
static bool collect(const pogdev_server_t *srv) {
  char url[256];
  snprintf(url, sizeof(url),
           "http://" IPSTR ":%u/api/v1/pogdev/announce/%s?secret=%s",
           IP2STR(&srv->addr), srv->api_port, s_hw_id, s_claim_secret);

  char resp[RESP_MAX];
  int status = http_call(url, "GET", NULL, resp, sizeof(resp));

  if (status == 404 || status == 410) {
    /* Inconnu du serveur, ou identifiants déjà remis à quelqu'un. Dans les deux
     * cas notre secret ne vaut plus rien : on repart d'une annonce neuve. */
    ESP_LOGW(TAG,
             "le serveur ne nous reconnaît plus (HTTP %d) — nouvelle annonce",
             status);
    s_claim_secret[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_key(h, NVS_KEY_CLAIM);
      nvs_commit(h);
      nvs_close(h);
    }
    return false;
  }
  if (status != 200) {
    return false;
  }

  cJSON *root = cJSON_Parse(resp);
  if (root == NULL) {
    return false;
  }
  const cJSON *st = cJSON_GetObjectItem(root, "status");
  bool adopted = cJSON_IsString(st) && strcmp(st->valuestring, "adopted") == 0;
  if (!adopted) {
    cJSON_Delete(root);
    return false;
  }

  const cJSON *dev = cJSON_GetObjectItem(root, "device_id");
  const cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
  const cJSON *host = mqtt ? cJSON_GetObjectItem(mqtt, "host") : NULL;
  const cJSON *port = mqtt ? cJSON_GetObjectItem(mqtt, "port") : NULL;
  const cJSON *pw = mqtt ? cJSON_GetObjectItem(mqtt, "password") : NULL;

  if (!cJSON_IsString(dev) || !cJSON_IsString(pw)) {
    cJSON_Delete(root);
    return false;
  }

  pogdev_creds_t c = {0};
  strlcpy(c.device_id, dev->valuestring, sizeof(c.device_id));
  strlcpy(c.mqtt_password, pw->valuestring, sizeof(c.mqtt_password));
  /* L'hôte annoncé peut être un nom ; on retient plutôt l'adresse déjà résolue
   * par la découverte, qui est celle par laquelle on vient de dialoguer. */
  if (cJSON_IsString(host) && strchr(host->valuestring, ':') == NULL) {
    strlcpy(c.mqtt_host, host->valuestring, sizeof(c.mqtt_host));
  }
  snprintf(c.mqtt_host, sizeof(c.mqtt_host), IPSTR, IP2STR(&srv->addr));
  c.mqtt_port = cJSON_IsNumber(port) ? (uint16_t)port->valuedouble : 1883;
  cJSON_Delete(root);

  /* Écrire AVANT de se déclarer adopté : les identifiants ne sont servis qu'une
   * fois, donc les perdre ici impose une ré-adoption manuelle. */
  esp_err_t err = store_creds(&c);
  if (err != ESP_OK) {
    ESP_LOGE(TAG,
             "impossible d'enregistrer les identifiants (%s) — on retentera",
             esp_err_to_name(err));
    return false;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_creds = c;
  s_have_creds = true;
  xSemaphoreGive(s_lock);

  ESP_LOGI(TAG, "adopté par pog Home — device_id=%s, mqtt %s:%u", c.device_id,
           c.mqtt_host, c.mqtt_port);
  return true;
}

static void enrol_task(void *arg) {
  (void)arg;
  build_hw_id();

  if (load_state()) {
    ESP_LOGI(TAG, "déjà enrôlé (device_id=%s)", s_creds.device_id);
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "pas encore enrôlé — identifiant matériel : %s", s_hw_id);

  int64_t started_ms = 0;
  for (;;) {
    pogdev_server_t srv;
    if (!pogdev_discovery_get(&srv)) {
      vTaskDelay(
          pdMS_TO_TICKS(5000)); /* la découverte n'a encore rien trouvé */
      continue;
    }
    if (ensure_claim_secret() != ESP_OK) {
      ESP_LOGE(TAG, "impossible de générer le secret d'enrôlement");
      vTaskDelay(pdMS_TO_TICKS(30000));
      continue;
    }

    /* Réannoncer à chaque tour : c'est ce qui garde la ligne vivante côté
     * serveur (elle expire au bout de 30 min) et met à jour « vu il y a … ». */
    announce(&srv);
    if (collect(&srv)) {
      break;
    }

    int delay_ms = POLL_FAST_MS;
    if (started_ms > POLL_MEDIUM_UNTIL_MS) {
      delay_ms = POLL_SLOW_MS;
    } else if (started_ms > POLL_FAST_UNTIL_MS) {
      delay_ms = POLL_MEDIUM_MS;
    }
    started_ms += delay_ms;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
  vTaskDelete(NULL);
}

esp_err_t pogdev_enrol_start(const char *device_name) {
  if (device_name != NULL) {
    strlcpy(s_device_name, device_name, sizeof(s_device_name));
  }
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  BaseType_t ok = xTaskCreate(enrol_task, "pogdev_enrol", 6144, NULL, 3, NULL);
  return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool pogdev_enrol_get(pogdev_creds_t *out) {
  if (out == NULL || s_lock == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool have = s_have_creds;
  if (have) {
    *out = s_creds;
  }
  xSemaphoreGive(s_lock);
  return have;
}

const char *pogdev_hw_id(void) {
  if (s_hw_id[0] == '\0') {
    build_hw_id();
  }
  return s_hw_id;
}

const char *pogdev_fw_version(void) {
  /* Le descripteur vit dans la flash mappée de l'image en cours : la chaîne
   * reste valable pour toute la durée d'exécution, donc pas de copie. */
  const esp_app_desc_t *app = esp_app_get_description();
  if (app == NULL || app->version[0] == '\0') {
    /* Ne peut arriver qu'avec une image sans descripteur. On renvoie un semver
     * valide plutôt que la chaîne vide : pog Home range ce champ dans
     * `sw_version` et doit pouvoir le comparer sans cas particulier. */
    return "0.0.0";
  }
  return app->version;
}
