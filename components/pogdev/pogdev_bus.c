/* pogdev — phase 3 : le bus MQTT.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §4. L'appareil publie un `hello`
 * retenu qui déclare ses traits, son `state` à chaque changement, et écoute
 * `cmd`. Sa disponibilité est portée par `status` avec un testament (LWT).
 *
 * Le lien ne renonce jamais (panne des 24-27 août 2026, enceintes muettes
 * jusqu'au débranchage) : esp-mqtt retente sans fin, la politique de
 * pogdev_retry.c décide quand suivre un serveur qui a déménagé et quand
 * relever des identifiants refusés — et une tâche de soin exécute ces gestes
 * hors des handlers d'événements, comme l'exige esp-mqtt.
 */
#include "pogdev_bus.h"
#include "pogdev_discovery.h"
#include "pogdev_enrol.h"
#include "pogdev_retry.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pogdev";

#define STATE_PERIOD_MS           30000
#define HELLO_RETRY_MS            5000
#define MQTT_RECONNECT_TIMEOUT_MS 60000
/* Quand relancer le client échoue (mémoire, transport), on réessaie à ce
 * rythme — jamais d'abandon : un appareil mural doit retrouver un serveur qui
 * revient des heures plus tard. */
#define RELANCE_PERIOD_MS 60000

/* Ordres portés par la tâche de soin. */
#define SOIN_RELEVE (1u << 0) /* identifiants refusés : demander au serveur */
#define SOIN_REPRISE \
  (1u << 1) /* échecs installés : le serveur a-t-il bougé ? */

static esp_mqtt_client_handle_t s_client;
static pogdev_creds_t s_creds;
static pogdev_cmd_handler s_handler;
static pogdev_describe_fn s_describe;
static pogdev_state_fn s_state;
static pogdev_effect_frame_handler s_effect_handler;
static bool s_effect_enabled;
static char s_topic_hello[96], s_topic_state[96], s_topic_status[96],
    s_topic_cmd[96];
static char s_effect_group[37], s_topic_effect[96];
static volatile pogdev_bus_status_t s_status = POGDEV_BUS_NOT_STARTED;
static pogdev_retry_t s_retry;
static TaskHandle_t s_soin;
static TaskHandle_t s_state_task_handle;
static atomic_bool s_hello_dirty = true;
/* Protège s_client pendant une bascule (arrêt/recréation par la tâche de
 * soin) contre les publications venues d'autres tâches. */
static SemaphoreHandle_t s_client_lock;

/* ---- le descripteur ----
 *
 * Le composant n'invente aucune entité : il pose l'enveloppe (protocole,
 * identité, local_rules) et laisse l'application remplir la liste. Un firmware
 * sait ce qu'il expose ; ce fichier ne peut pas le savoir.
 */
static char *build_hello(void) {
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return NULL;
  }
  cJSON_AddNumberToObject(root, "proto", 1);
  cJSON_AddStringToObject(root, "hw_id", pogdev_hw_id());
  cJSON_AddStringToObject(root, "model", POGDEV_MODEL);
  cJSON_AddStringToObject(root, "fw_version", pogdev_fw_version());
  if (s_effect_enabled) {
    cJSON *features = cJSON_AddArrayToObject(root, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("effect_sync_v1"));
  }

  cJSON *entities = cJSON_AddArrayToObject(root, "entities");
  if (s_describe != NULL) {
    s_describe(entities);
  }

  /* Réservé dès la v1 même vide : c'est le champ qu'on ne pourra pas ajouter
   * plus tard sans reflasher tout le parc. */
  cJSON_AddItemToObject(root, "local_rules", cJSON_CreateArray());

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

/* QoS 1 reprend un message une fois accepté par la file esp-mqtt. En revanche,
 * publish() renvoie -1 si cette mise en file locale échoue ; sans ce drapeau,
 * une enceinte restait connectée mais invisible jusqu'à sa prochaine coupure.
 */
static bool publish_hello(void) {
  if (!atomic_exchange(&s_hello_dirty, false)) {
    return true;
  }

  char *hello = build_hello();
  if (hello == NULL || s_client_lock == NULL) {
    cJSON_free(hello);
    atomic_store(&s_hello_dirty, true);
    return false;
  }

  bool published = false;
  if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (s_client != NULL && s_status == POGDEV_BUS_CONNECTED) {
      int message_id = esp_mqtt_client_publish(
          s_client, s_topic_hello, hello, 0, 1, 1 /* retenu */);
      published = message_id >= 0;
      if (published) {
        ESP_LOGI(TAG, "manifeste mis en file (id %d)", message_id);
      }
    }
    xSemaphoreGive(s_client_lock);
  }
  cJSON_free(hello);

  if (!published) {
    atomic_store(&s_hello_dirty, true);
    ESP_LOGW(TAG, "manifeste refuse par la file MQTT ; nouvel essai prevu");
  }
  return published;
}

static void publish_state(void) {
  if (s_state == NULL || s_client_lock == NULL) {
    return;
  }
  /* Délai court et abandon silencieux : pendant une bascule, l'état est de
   * toute façon reperdu par le broker, et le tick suivant rattrape. Attendre
   * ici depuis la tâche du client MQTT bloquerait l'arrêt que la bascule
   * attend — l'interblocage classique. */
  if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  if (s_client != NULL) {
    cJSON *root = cJSON_CreateObject();
    s_state(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != NULL) {
      esp_mqtt_client_publish(s_client, s_topic_state, json, 0, 0,
                              1 /* retenu */);
      cJSON_free(json);
    }
  }
  xSemaphoreGive(s_client_lock);
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
    ESP_LOGI(TAG, "commande reçue : %s.%s", key->valuestring,
             name->valuestring);
    if (s_handler != NULL) {
      s_handler(key->valuestring, name->valuestring,
                cJSON_GetObjectItem(root, "params"));
    }
    /* Confirmer par l'état, jamais par une réponse sur `cmd` : côté serveur,
     * « la commande a été émise » et « l'appareil a obéi » sont deux faits
     * distincts, et le second n'arrive que par `state`. */
    publish_state();
  }
  cJSON_Delete(root);
}

static bool connack_refuse_identifiants(const esp_mqtt_event_handle_t ev) {
  /* Le type d'erreur d'abord : sur un incident de transport, le code CONNACK
   * du handle est un reliquat du refus précédent, et le lire seul ferait
   * passer une coupure réseau pour un compte révoqué. */
  if (ev->error_handle == NULL ||
      ev->error_handle->error_type != MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    return false;
  }
  esp_mqtt_connect_return_code_t code = ev->error_handle->connect_return_code;
  return code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ||
         code == MQTT_CONNECTION_REFUSE_BAD_USERNAME;
}

static void mqtt_event(void *args, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)args;
  (void)base;
  esp_mqtt_event_handle_t ev = data;

  switch ((esp_mqtt_event_id_t)id) {
  case MQTT_EVENT_CONNECTED: {
    s_status = POGDEV_BUS_CONNECTED;
    pogdev_retry_connecte(&s_retry);
    ESP_LOGI(TAG, "connecté au broker %s:%u", s_creds.mqtt_host,
             s_creds.mqtt_port);
    esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, 1);
    /* Republier à CHAQUE reconnexion : le broker peut avoir perdu ses retenus
     * pendant que l'enceinte, elle, conserve son adoption en NVS. */
    atomic_store(&s_hello_dirty, true);
    publish_hello();
    esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
    if (s_topic_effect[0] != '\0') {
      esp_mqtt_client_subscribe(s_client, s_topic_effect, 0);
    }
    publish_state();
    break;
  }
  case MQTT_EVENT_DATA:
    if (s_topic_effect[0] != '\0' &&
        ev->topic_len == (int)strlen(s_topic_effect) &&
        memcmp(ev->topic, s_topic_effect, (size_t)ev->topic_len) == 0) {
      if (s_effect_handler != NULL) s_effect_handler(ev->data, ev->data_len);
    } else if (ev->topic_len == (int)strlen(s_topic_cmd) &&
               memcmp(ev->topic, s_topic_cmd, (size_t)ev->topic_len) == 0) {
      on_command(ev->data, ev->data_len);
    }
    break;
  case MQTT_EVENT_ERROR:
    /* Un refus d'authentification n'est PAS un ordre d'oubli : pendant que
     * pog Home redémarre, le broker refuse aussi avec des comptes pas encore
     * reprovisionnés, et la ré-adoption d'un appareil effacé exige un humain.
     * Quand les refus persistent, la relève (secret de réclamation en NVS)
     * demande au serveur — c'est lui qui tranche : 404, « pending » visible
     * dans l'inventaire, ou identifiants neufs. */
    if (connack_refuse_identifiants(ev)) {
      s_status = POGDEV_BUS_AUTH_FAILED;
      ESP_LOGE(TAG, "identifiants MQTT refusés — relève auprès de pog Home "
                    "si cela persiste");
      if (s_soin != NULL &&
          pogdev_retry_refus_auth(&s_retry,
                                  (uint32_t)(esp_timer_get_time() / 1000))) {
        xTaskNotify(s_soin, SOIN_RELEVE, eSetBits);
      }
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    atomic_store(&s_hello_dirty, true);
    if (s_status != POGDEV_BUS_AUTH_FAILED) {
      s_status = POGDEV_BUS_CONNECTING;
    }
    /* Le serveur a peut-être déménagé : l'adresse gelée à l'adoption a déjà
     * survécu à un déménagement de pog Home (26 août 2026) pendant que la
     * découverte, elle, connaissait la nouvelle. La tâche de soin compare et
     * ne bascule que si quelque chose a vraiment changé. */
    if (s_soin != NULL && pogdev_retry_echec(&s_retry)) {
      xTaskNotify(s_soin, SOIN_REPRISE, eSetBits);
    }
    break;
  default:
    break;
  }
}

static void state_task(void *arg) {
  (void)arg;
  TickType_t last_state = xTaskGetTickCount();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(HELLO_RETRY_MS));
    if (atomic_load(&s_hello_dirty) &&
        s_status == POGDEV_BUS_CONNECTED) {
      publish_hello();
    }
    TickType_t now = xTaskGetTickCount();
    if (now - last_state >= pdMS_TO_TICKS(STATE_PERIOD_MS)) {
      last_state = now;
      publish_state();
    }
  }
}

/* (Re)crée et démarre le client MQTT depuis les identifiants en NVS. Appelée
 * au démarrage puis à chaque bascule ; suppose que s_client est NULL. */
static esp_err_t bus_launch(void) {
  if (!pogdev_enrol_get(&s_creds)) {
    s_status = POGDEV_BUS_NOT_STARTED;
    return ESP_ERR_INVALID_STATE; /* pas encore adopté */
  }

  snprintf(s_topic_hello, sizeof(s_topic_hello), "pog/%s/hello",
           s_creds.device_id);
  snprintf(s_topic_state, sizeof(s_topic_state), "pog/%s/state",
           s_creds.device_id);
  snprintf(s_topic_status, sizeof(s_topic_status), "pog/%s/status",
           s_creds.device_id);
  snprintf(s_topic_cmd, sizeof(s_topic_cmd), "pog/%s/cmd", s_creds.device_id);

  char uri[80];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", s_creds.mqtt_host,
           s_creds.mqtt_port);

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
      .network.reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
  };

  s_status = POGDEV_BUS_CONNECTING;
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
  if (client == NULL) {
    s_status = POGDEV_BUS_NOT_STARTED;
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                 mqtt_event, NULL);
  if (err == ESP_OK) {
    err = esp_mqtt_client_start(client);
  }
  if (err != ESP_OK) {
    esp_mqtt_client_destroy(client);
    s_status = POGDEV_BUS_NOT_STARTED;
    return err;
  }
  s_client = client;
  return ESP_OK;
}

/* Arrête le client puis le relance sur les identifiants et l'adresse frais.
 * Vit dans la tâche de soin : esp-mqtt interdit stop() depuis ses handlers. */
static void bus_relancer(void) {
  xSemaphoreTake(s_client_lock, portMAX_DELAY);
  if (s_client != NULL) {
    /* stop() d'abord : il attend la fin de la tâche du client, donc plus
     * aucun handler ne court quand le handle devient NULL puis disparaît. */
    esp_mqtt_client_handle_t vieux = s_client;
    esp_mqtt_client_stop(vieux);
    s_client = NULL;
    esp_mqtt_client_destroy(vieux);
  }
  pogdev_retry_init(&s_retry);
  while (bus_launch() != ESP_OK) {
    /* Relancer peut échouer (mémoire) : réessayer, jamais renoncer — le
     * WARN d'un seul essai perdu est exactement ce qui a laissé des
     * enceintes saines et muettes pendant des jours. */
    ESP_LOGW(TAG, "relance du bus impossible, nouvel essai dans %u s",
             (unsigned)(RELANCE_PERIOD_MS / 1000));
    vTaskDelay(pdMS_TO_TICKS(RELANCE_PERIOD_MS));
  }
  xSemaphoreGive(s_client_lock);
}

static void soin_task(void *arg) {
  (void)arg;
  for (;;) {
    uint32_t ordres = 0;
    xTaskNotifyWait(0, UINT32_MAX, &ordres, portMAX_DELAY);
    bool relancer = false;

    if (ordres & SOIN_RELEVE) {
      if (pogdev_enrol_recollect()) {
        ESP_LOGI(TAG, "identifiants relevés à neuf, on repart avec");
        relancer = true;
      }
    }
    if (!relancer && (ordres & SOIN_REPRISE)) {
      pogdev_server_t srv;
      if (pogdev_discovery_get(&srv) &&
          pogdev_enrol_update_host(&srv.addr, srv.mqtt_port)) {
        relancer = true;
      }
    }
    if (relancer) {
      bus_relancer();
    }
  }
}

void pogdev_bus_notify(void) {
  publish_state();
}

void pogdev_bus_enable_effect_sync(pogdev_effect_frame_handler handler) {
  if (s_client != NULL) return;
  s_effect_enabled = handler != NULL;
  s_effect_handler = handler;
}

esp_err_t pogdev_bus_effect_sync_join(const char *group_id) {
  if (!s_effect_enabled || group_id == NULL || strlen(group_id) != 36 ||
      s_client_lock == NULL) return ESP_ERR_INVALID_ARG;
  char next[sizeof(s_topic_effect)];
  int length = snprintf(next, sizeof(next), "pog/effects/%s/frame", group_id);
  if (length < 0 || length >= (int)sizeof(next)) return ESP_ERR_INVALID_SIZE;
  if (xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(250)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  esp_err_t result = ESP_ERR_INVALID_STATE;
  if (s_client != NULL && s_status == POGDEV_BUS_CONNECTED) {
    int id = esp_mqtt_client_subscribe(s_client, next, 0);
    if (id >= 0) {
      char old[sizeof(s_topic_effect)];
      memcpy(old, s_topic_effect, sizeof(old));
      memcpy(s_topic_effect, next, sizeof(next));
      memcpy(s_effect_group, group_id, 37);
      if (old[0] != '\0' && strcmp(old, next) != 0)
        esp_mqtt_client_unsubscribe(s_client, old);
      result = ESP_OK;
    }
  }
  xSemaphoreGive(s_client_lock);
  return result;
}

bool pogdev_bus_effect_sync_leave(const char *group_id) {
  if (group_id == NULL || strcmp(group_id, s_effect_group) != 0) return false;
  pogdev_bus_effect_sync_cancel();
  return true;
}

void pogdev_bus_effect_sync_cancel(void) {
  if (s_client_lock == NULL ||
      xSemaphoreTake(s_client_lock, pdMS_TO_TICKS(250)) != pdTRUE) return;
  if (s_client != NULL && s_topic_effect[0] != '\0')
    esp_mqtt_client_unsubscribe(s_client, s_topic_effect);
  s_effect_group[0] = '\0';
  s_topic_effect[0] = '\0';
  xSemaphoreGive(s_client_lock);
}

pogdev_bus_status_t pogdev_bus_get_status(void) {
  return s_status;
}

esp_err_t pogdev_bus_start(pogdev_describe_fn describe, pogdev_state_fn state,
                           pogdev_cmd_handler handler) {
  s_describe = describe;
  s_state = state;
  s_handler = handler;
  pogdev_retry_init(&s_retry);

  if (s_client_lock == NULL) {
    s_client_lock = xSemaphoreCreateMutex();
    if (s_client_lock == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  esp_err_t err = bus_launch();
  if (err != ESP_OK) {
    return err;
  }
  if (s_state_task_handle == NULL &&
      xTaskCreate(state_task, "pogdev_state", 3072, NULL, 2,
                  &s_state_task_handle) != pdPASS) {
    s_state_task_handle = NULL;
    ESP_LOGW(TAG, "tâche de publication non créée : état sur changement et "
                  "manifeste repris seulement à la reconnexion");
  }
  if (s_soin == NULL &&
      xTaskCreate(soin_task, "pogdev_soin", 4096, NULL, 2, &s_soin) != pdPASS) {
    s_soin = NULL;
    ESP_LOGW(TAG, "tâche de soin non créée : pas de suivi de déménagement ni "
                  "de relève");
  }
  return ESP_OK;
}
