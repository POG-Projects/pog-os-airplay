/* pogdev — phase 3 : le bus MQTT.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §4. L'appareil publie un `hello`
 * retenu qui déclare ses traits, son `state` à chaque changement, et écoute
 * `cmd`. Sa disponibilité est portée par `status` avec un testament (LWT).
 */
#include "pogdev_bus.h"
#include "pogdev_enrol.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pogdev";

#define STATE_PERIOD_MS 30000

static esp_mqtt_client_handle_t s_client;
static pogdev_creds_t s_creds;
static pogdev_cmd_handler s_handler;
static char s_topic_hello[96], s_topic_state[96], s_topic_status[96], s_topic_cmd[96];

/* ---- le descripteur ----
 *
 * Ce que l'enceinte déclare savoir faire. Deux entités seulement pour l'instant,
 * mais chacune est réelle :
 *
 *  - `transport` porte le trait `action`, dont les commandes sont déclarées ICI
 *    plutôt que dans le registre du serveur. C'est la raison d'être des traits
 *    dynamiques : le vocabulaire d'un transport est propre à l'appareil, comme
 *    celui d'une sirène Zigbee. Elles sont marquées réversibles parce qu'elles
 *    le sont vraiment — play_pause bascule, volume_up s'annule par volume_down —
 *    ce qui les garde au niveau « act » et non « act_sensitive ».
 *
 *  - `wifi` est un diagnostic. Le serveur ne l'exposera pas à l'assistant, sa
 *    liste de `kind` visibles ne contient pas `rssi` : un capteur de qualité de
 *    signal n'a rien à faire dans le contexte d'un modèle.
 */
static char *build_hello(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "proto", 1);
  cJSON_AddStringToObject(root, "hw_id", pogdev_hw_id());
  cJSON_AddStringToObject(root, "model", "POG AirPlay (XIAO S3)");
  cJSON_AddStringToObject(root, "fw_version", POGDEV_FW_VERSION);

  cJSON *entities = cJSON_AddArrayToObject(root, "entities");

  /* transport */
  cJSON *transport = cJSON_CreateObject();
  cJSON_AddStringToObject(transport, "key", "transport");
  cJSON_AddStringToObject(transport, "name", "Lecture");
  cJSON *t_traits = cJSON_AddArrayToObject(transport, "traits");
  cJSON *action = cJSON_CreateObject();
  cJSON_AddStringToObject(action, "id", "action");
  cJSON *cfg = cJSON_AddObjectToObject(action, "config");
  cJSON *cmds = cJSON_AddArrayToObject(cfg, "commands");
  static const char *names[] = {"play_pause", "next", "prev", "volume_up", "volume_down"};
  static const char *labels[] = {"Lecture / pause", "Suivant", "Précédent", "Volume +", "Volume −"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "name", names[i]);
    cJSON_AddStringToObject(c, "label", labels[i]);
    cJSON_AddBoolToObject(c, "reversible", true);
    cJSON_AddItemToArray(cmds, c);
  }
  cJSON_AddItemToArray(t_traits, action);
  cJSON_AddItemToArray(entities, transport);

  /* wifi */
  cJSON *wifi = cJSON_CreateObject();
  cJSON_AddStringToObject(wifi, "key", "wifi");
  cJSON_AddStringToObject(wifi, "name", "Signal WiFi");
  cJSON *w_traits = cJSON_AddArrayToObject(wifi, "traits");
  cJSON *meas = cJSON_CreateObject();
  cJSON_AddStringToObject(meas, "id", "measurement");
  cJSON *w_cfg = cJSON_AddObjectToObject(meas, "config");
  cJSON_AddStringToObject(w_cfg, "kind", "rssi");
  cJSON_AddStringToObject(w_cfg, "unit", "dBm");
  cJSON_AddItemToArray(w_traits, meas);
  cJSON_AddItemToArray(entities, wifi);

  /* Réservé dès la v1 même vide : c'est le champ qu'on ne pourra pas ajouter
   * plus tard sans reflasher tout le parc. */
  cJSON_AddItemToObject(root, "local_rules", cJSON_CreateArray());

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

static void publish_state(void) {
  if (s_client == NULL) {
    return;
  }
  wifi_ap_record_t ap;
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    rssi = ap.rssi;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  cJSON_AddNumberToObject(wifi, "value", rssi);
  cJSON_AddStringToObject(wifi, "kind", "rssi");
  /* `transport` n'a pas d'état : le trait `action` est en écriture seule. On le
   * publie quand même vide, pour que le serveur voie l'entité vivante. */
  cJSON_AddItemToObject(root, "transport", cJSON_CreateObject());

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    return;
  }
  esp_mqtt_client_publish(s_client, s_topic_state, json, 0, 0, 1 /* retenu */);
  cJSON_free(json);
}

static void on_command(const char *data, int len) {
  cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
  if (root == NULL) {
    ESP_LOGW(TAG, "commande illisible");
    return;
  }
  const cJSON *key = cJSON_GetObjectItem(root, "key");
  const cJSON *name = cJSON_GetObjectItem(root, "name");
  if (cJSON_IsString(key) && cJSON_IsString(name)) {
    ESP_LOGI(TAG, "commande reçue : %s.%s", key->valuestring, name->valuestring);
    if (s_handler != NULL) {
      s_handler(key->valuestring, name->valuestring, cJSON_GetObjectItem(root, "params"));
    }
    /* Confirmer par l'état, jamais par une réponse sur `cmd` : côté serveur,
     * « la commande a été émise » et « l'appareil a obéi » sont deux faits
     * distincts, et le second n'arrive que par `state`. */
    publish_state();
  }
  cJSON_Delete(root);
}

static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data) {
  (void)args;
  (void)base;
  esp_mqtt_event_handle_t ev = data;

  switch ((esp_mqtt_event_id_t)id) {
  case MQTT_EVENT_CONNECTED: {
    ESP_LOGI(TAG, "connecté au broker %s:%u", s_creds.mqtt_host, s_creds.mqtt_port);
    esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, 1);
    char *hello = build_hello();
    if (hello != NULL) {
      esp_mqtt_client_publish(s_client, s_topic_hello, hello, 0, 1, 1 /* retenu */);
      cJSON_free(hello);
    }
    esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
    publish_state();
    break;
  }
  case MQTT_EVENT_DATA:
    on_command(ev->data, ev->data_len);
    break;
  case MQTT_EVENT_ERROR:
    /* Un refus d'authentification veut dire que l'appareil a été supprimé côté
     * serveur : le compte ne reviendra pas, et réessayer en boucle ne sert à
     * rien. On le dit clairement plutôt que de laisser la pile réessayer en
     * silence. */
    if (ev->error_handle != NULL &&
        ev->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
      ESP_LOGE(TAG, "identifiants MQTT refusés — l'appareil a probablement été "
                    "supprimé de pog Home ; efface la NVS pour te réannoncer");
    }
    break;
  default:
    break;
  }
}

static void state_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(STATE_PERIOD_MS));
    publish_state();
  }
}

esp_err_t pogdev_bus_start(pogdev_cmd_handler handler) {
  if (!pogdev_enrol_get(&s_creds)) {
    return ESP_ERR_INVALID_STATE; /* pas encore adopté */
  }
  s_handler = handler;

  snprintf(s_topic_hello, sizeof(s_topic_hello), "pog/%s/hello", s_creds.device_id);
  snprintf(s_topic_state, sizeof(s_topic_state), "pog/%s/state", s_creds.device_id);
  snprintf(s_topic_status, sizeof(s_topic_status), "pog/%s/status", s_creds.device_id);
  snprintf(s_topic_cmd, sizeof(s_topic_cmd), "pog/%s/cmd", s_creds.device_id);

  char uri[80];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", s_creds.mqtt_host, s_creds.mqtt_port);

  esp_mqtt_client_config_t cfg = {
      .broker.address.uri = uri,
      .credentials.username = s_creds.device_id,
      .credentials.client_id = s_creds.device_id,
      .credentials.authentication.password = s_creds.mqtt_password,
      /* Le testament : si la connexion tombe, le broker publie « offline » à
       * notre place. C'est ce qui distingue un appareil muet d'un appareil
       * absent, et donc ce qui évite qu'une automatisation lise un état figé
       * comme s'il était courant. */
      .session.last_will.topic = s_topic_status,
      .session.last_will.msg = "offline",
      .session.last_will.msg_len = 7,
      .session.last_will.qos = 1,
      .session.last_will.retain = 1,
      .session.keepalive = 30,
  };

  s_client = esp_mqtt_client_init(&cfg);
  if (s_client == NULL) {
    return ESP_FAIL;
  }
  esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
  if (err != ESP_OK) {
    return err;
  }
  err = esp_mqtt_client_start(s_client);
  if (err != ESP_OK) {
    return err;
  }
  xTaskCreate(state_task, "pogdev_state", 3072, NULL, 2, NULL);
  return ESP_OK;
}
