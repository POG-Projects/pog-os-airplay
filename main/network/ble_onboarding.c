/* Transport NimBLE du protocole d'accessoires POG (voir onboarding.h).
 *
 * Le contrat GATT est celui que pog Console connaît déjà pour Jarvis — même
 * service, mêmes caractéristiques, même cadrage — pour qu'un seul code iOS
 * configure toute la famille :
 *
 *   service    7A110000-504F-472D-4A41-525649530001
 *   info       …0001  lecture chiffrée : le lien pogconsole://provision
 *   provision  …0002  écriture chiffrée, cadrée 0x01 (en-tête) / 0x02 (suite)
 *   status     …0003  lecture + notification : {"status", "message"?}
 *   networks   …0004  lecture + notification : {"scanning", "networks"[]}
 *
 * Toutes les caractéristiques exigent le chiffrement du lien : c'est ce qui
 * force le jumelage (Just Works) au premier accès, et donc ce qui fait
 * apparaître la feuille AccessorySetupKit côté iOS. Les secrets (mot de passe
 * Wi-Fi, assertion) ne voyagent jamais en clair dans l'air. */
#include "ble_onboarding.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "onboarding.h"
#include "wifi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_onboard";

/* Le nom annoncé, celui que la feuille AccessorySetupKit montre à l'humain. */
#define BLE_DEVICE_NAME "POG AirPlay"

/* Cadrage des écritures `provision` : 0x01 ouvre (taille sur deux octets),
 * 0x02 continue. Une écriture BLE ne dépasse jamais MTU-3, donc 512 couvre le
 * MTU négocié. La réponse `networks` est bornée comme côté Jarvis : au-delà,
 * iOS lit en plusieurs fois et une liste écourtée reste utilisable. */
#define FRAME_HEADER       0x01
#define FRAME_CONTINUATION 0x02
#define PAYLOAD_MAX        4096
#define FRAME_MAX          512
#define STATUS_JSON_MAX    192
#define NETWORKS_JSON_MAX  500
#define RESTART_DELAY_US   (700 * 1000)

/* UUID en petit-boutiste, comme NimBLE les range. Seul l'octet 12 change
 * d'une caractéristique à l'autre (7A1100xx-…). */
#define POG_UUID128(byte12)                                                    \
  BLE_UUID128_INIT(0x01, 0x00, 0x53, 0x49, 0x56, 0x52, 0x41, 0x4A, 0x2D, 0x47, \
                   0x4F, 0x50, (byte12), 0x00, 0x11, 0x7A)

static const ble_uuid128_t s_svc_uuid = POG_UUID128(0x00);
static const ble_uuid128_t s_info_uuid = POG_UUID128(0x01);
static const ble_uuid128_t s_provision_uuid = POG_UUID128(0x02);
static const ble_uuid128_t s_status_uuid = POG_UUID128(0x03);
static const ble_uuid128_t s_networks_uuid = POG_UUID128(0x04);

/* Fourni par le port ESP-IDF de NimBLE (stockage des clés de jumelage en
 * NVS) ; déclaré ici faute d'en-tête public, comme dans les exemples IDF. */
void ble_store_config_init(void);

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_val_handle;
static uint16_t s_networks_val_handle;

/* Réassemblage des écritures `provision`. Un seul central (voir sdkconfig) :
 * pas d'entrelacement possible. */
static char s_incoming[PAYLOAD_MAX + 1];
static size_t s_incoming_expected;
static size_t s_incoming_len;

/* Le flux `authorize` puis `network` garde l'assertion en RAM entre les deux
 * écritures ; elle disparaît à la déconnexion. */
static char s_pending_assertion[PAYLOAD_MAX + 1];
static char s_pending_challenge[64];

/* Derniers JSON publiés, servis tels quels aux lectures. Le mutex arbitre la
 * tâche de scan Wi-Fi et la tâche hôte NimBLE. */
static SemaphoreHandle_t s_json_lock;
static char s_status_json[STATUS_JSON_MAX];
static char s_networks_json[NETWORKS_JSON_MAX + 8];

static esp_timer_handle_t s_restart_timer;

static void notify_value(uint16_t val_handle, const char *json) {
  if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
  if (om != NULL) {
    ble_gatts_notify_custom(s_conn_handle, val_handle, om);
  }
}

static void publish_status(const char *status, const char *message) {
  cJSON *doc = cJSON_CreateObject();
  cJSON_AddStringToObject(doc, "status", status);
  if (message != NULL && message[0] != '\0') {
    cJSON_AddStringToObject(doc, "message", message);
  }
  char *json = cJSON_PrintUnformatted(doc);
  cJSON_Delete(doc);
  if (json == NULL) {
    return;
  }
  xSemaphoreTake(s_json_lock, portMAX_DELAY);
  strlcpy(s_status_json, json, sizeof(s_status_json));
  xSemaphoreGive(s_json_lock);
  cJSON_free(json);
  ESP_LOGI(TAG, "statut=%s%s%s", status, message != NULL ? " · " : "",
           message != NULL ? message : "");
  notify_value(s_status_val_handle, s_status_json);
}

static void publish_networks(bool scanning, const wifi_ap_record_t *records,
                             uint16_t count) {
  cJSON *doc = cJSON_CreateObject();
  cJSON_AddBoolToObject(doc, "scanning", scanning);
  cJSON *networks = cJSON_AddArrayToObject(doc, "networks");
  for (uint16_t i = 0; !scanning && networks != NULL && i < count; i++) {
    const char *ssid = (const char *)records[i].ssid;
    if (ssid[0] == '\0') {
      continue;
    }
    bool seen = false;
    cJSON *existing = NULL;
    cJSON_ArrayForEach(existing, networks) {
      const cJSON *name = cJSON_GetObjectItem(existing, "ssid");
      if (cJSON_IsString(name) && strcmp(name->valuestring, ssid) == 0) {
        seen = true;
        break;
      }
    }
    if (seen) {
      continue;
    }
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", ssid);
    cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
    cJSON_AddBoolToObject(item, "secure",
                          records[i].authmode != WIFI_AUTH_OPEN);
    cJSON_AddItemToArray(networks, item);
    /* Comme côté Jarvis : on retire le dernier réseau qui ferait déborder la
     * réponse plutôt que de servir un JSON tronqué. */
    char *candidate = cJSON_PrintUnformatted(doc);
    if (candidate == NULL) {
      break;
    }
    size_t len = strlen(candidate);
    cJSON_free(candidate);
    if (len > NETWORKS_JSON_MAX) {
      cJSON_DeleteItemFromArray(networks, cJSON_GetArraySize(networks) - 1);
      break;
    }
  }
  char *json = cJSON_PrintUnformatted(doc);
  cJSON_Delete(doc);
  if (json == NULL) {
    return;
  }
  xSemaphoreTake(s_json_lock, portMAX_DELAY);
  strlcpy(s_networks_json, json, sizeof(s_networks_json));
  xSemaphoreGive(s_json_lock);
  cJSON_free(json);
  notify_value(s_networks_val_handle, s_networks_json);
}

static void restart_timer_cb(void *arg) {
  (void)arg;
  esp_restart();
}

static void reset_incoming(void) {
  s_incoming_expected = 0;
  s_incoming_len = 0;
  s_incoming[0] = '\0';
}

static void clear_pending_authorization(void) {
  memset(s_pending_assertion, 0, sizeof(s_pending_assertion));
  s_pending_challenge[0] = '\0';
}

/* Un JSON complet est arrivé sur `provision`. Trois requêtes possibles,
 * celles que pog Console émet : `authorize` (déposer l'assertion), `network`
 * (choisir le réseau après une autorisation), `provision` (tout d'un coup —
 * le flux actuel de l'app). */
static void apply_payload(void) {
  ESP_LOGI(TAG, "configuration reçue (%u octets)", (unsigned)s_incoming_len);
  cJSON *doc = cJSON_Parse(s_incoming);
  if (doc == NULL) {
    publish_status("error", "Requête BLE invalide.");
    return;
  }
  const cJSON *type_item = cJSON_GetObjectItem(doc, "type");
  const char *type =
      cJSON_IsString(type_item) ? type_item->valuestring : "provision";
  const cJSON *challenge_item = cJSON_GetObjectItem(doc, "challenge");
  const char *challenge =
      cJSON_IsString(challenge_item) ? challenge_item->valuestring : "";
  const cJSON *assertion_item = cJSON_GetObjectItem(doc, "assertion");
  const char *assertion =
      cJSON_IsString(assertion_item) ? assertion_item->valuestring : "";

  if (strcmp(type, "authorize") == 0) {
    size_t len = strlen(assertion);
    if (strcmp(challenge, onboarding_challenge()) != 0 || len < 128 ||
        len > PAYLOAD_MAX || strchr(assertion, '.') == NULL) {
      publish_status("error", "Autorisation POG Auth invalide.");
    } else {
      strlcpy(s_pending_challenge, challenge, sizeof(s_pending_challenge));
      strlcpy(s_pending_assertion, assertion, sizeof(s_pending_assertion));
      publish_status("authorized", NULL);
    }
    cJSON_Delete(doc);
    return;
  }

  if (strcmp(type, "network") == 0) {
    if (strcmp(challenge, s_pending_challenge) != 0 ||
        s_pending_assertion[0] == '\0') {
      publish_status("error", "Autorisez d’abord l’accessoire dans POG Auth.");
      cJSON_Delete(doc);
      return;
    }
    assertion = s_pending_assertion;
  }

  const cJSON *ssid_item = cJSON_GetObjectItem(doc, "ssid");
  const cJSON *password_item = cJSON_GetObjectItem(doc, "password");
  char error[96] = {0};
  bool ok = onboarding_apply(
      challenge, cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL,
      cJSON_IsString(password_item) ? password_item->valuestring : "",
      assertion, error, sizeof(error));
  cJSON_Delete(doc);
  if (!ok) {
    publish_status("error", error);
    return;
  }
  clear_pending_authorization();
  /* `joining` part en notification, puis l'appareil redémarre : c'est le
   * prochain démarrage, credentials en poche, qui rejoint le réseau et se
   * fait adopter. Le délai laisse la notification quitter la pile. */
  publish_status("joining", NULL);
  esp_timer_start_once(s_restart_timer, RESTART_DELAY_US);
}

static void handle_provision_frame(const uint8_t *bytes, size_t len) {
  if (len == 0) {
    return;
  }
  if (bytes[0] == FRAME_HEADER) {
    if (len < 3) {
      publish_status("error", "Trame BLE trop courte.");
      return;
    }
    size_t expected = ((size_t)bytes[1] << 8) | bytes[2];
    if (expected == 0 || expected > PAYLOAD_MAX) {
      reset_incoming();
      publish_status("error", "Configuration BLE trop volumineuse.");
      return;
    }
    s_incoming_expected = expected;
    s_incoming_len = 0;
    len -= 3;
    bytes += 3;
  } else if (bytes[0] == FRAME_CONTINUATION && s_incoming_expected > 0) {
    len -= 1;
    bytes += 1;
  } else {
    reset_incoming();
    publish_status("error", "Séquence BLE invalide.");
    return;
  }
  if (s_incoming_len + len > s_incoming_expected) {
    reset_incoming();
    publish_status("error", "Configuration BLE corrompue.");
    return;
  }
  memcpy(s_incoming + s_incoming_len, bytes, len);
  s_incoming_len += len;
  s_incoming[s_incoming_len] = '\0';
  if (s_incoming_len == s_incoming_expected) {
    apply_payload();
    reset_incoming();
  }
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;
  const ble_uuid_t *uuid = ctxt->chr->uuid;

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (ble_uuid_cmp(uuid, &s_info_uuid.u) == 0) {
      char link[256];
      onboarding_deep_link(link, sizeof(link));
      return os_mbuf_append(ctxt->om, link, strlen(link)) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    const char *json = NULL;
    if (ble_uuid_cmp(uuid, &s_status_uuid.u) == 0) {
      json = s_status_json;
    } else if (ble_uuid_cmp(uuid, &s_networks_uuid.u) == 0) {
      json = s_networks_json;
    }
    if (json != NULL) {
      xSemaphoreTake(s_json_lock, portMAX_DELAY);
      int rc = os_mbuf_append(ctxt->om, json, strlen(json));
      xSemaphoreGive(s_json_lock);
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
      ble_uuid_cmp(uuid, &s_provision_uuid.u) == 0) {
    uint8_t frame[FRAME_MAX];
    uint16_t frame_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &frame_len) != 0) {
      return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    handle_provision_frame(frame, frame_len);
    return 0;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_info_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                },
                {
                    .uuid = &s_provision_uuid.u,
                    .access_cb = gatt_access_cb,
                    .flags = BLE_GATT_CHR_F_WRITE |
                             BLE_GATT_CHR_F_WRITE_NO_RSP |
                             BLE_GATT_CHR_F_WRITE_ENC,
                },
                {
                    .uuid = &s_status_uuid.u,
                    .access_cb = gatt_access_cb,
                    .val_handle = &s_status_val_handle,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
                },
                {
                    .uuid = &s_networks_uuid.u,
                    .access_cb = gatt_access_cb,
                    .val_handle = &s_networks_val_handle,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
                },
                {0},
            },
    },
    {0},
};

static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      s_conn_handle = event->connect.conn_handle;
      reset_incoming();
      clear_pending_authorization();
      publish_status("ready", NULL);
    } else {
      start_advertising();
    }
    return 0;
  case BLE_GAP_EVENT_DISCONNECT:
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    reset_incoming();
    clear_pending_authorization();
    start_advertising();
    return 0;
  case BLE_GAP_EVENT_ADV_COMPLETE:
    start_advertising();
    return 0;
  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    /* L'iPhone a oublié le lien (réinitialisation, changement d'appareil) et
     * se représente : on efface l'ancien secret et on laisse le jumelage
     * reprendre, sinon il échoue en boucle des deux côtés. */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
      ble_store_util_delete_peer(&desc.peer_id_addr);
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }
  case BLE_GAP_EVENT_ENC_CHANGE:
    ESP_LOGI(TAG, "chiffrement du lien : %s",
             event->enc_change.status == 0 ? "établi" : "refusé");
    return 0;
  default:
    return 0;
  }
}

static void start_advertising(void) {
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &s_svc_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
    return;
  }

  /* L'UUID 128 bits remplit l'annonce ; le nom part dans la réponse au scan,
   * là où AccessorySetupKit va le chercher. */
  struct ble_hs_adv_fields rsp;
  memset(&rsp, 0, sizeof(rsp));
  rsp.name = (const uint8_t *)BLE_DEVICE_NAME;
  rsp.name_len = strlen(BLE_DEVICE_NAME);
  rsp.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc != 0) {
    ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
    return;
  }

  struct ble_gap_adv_params params;
  memset(&params, 0, sizeof(params));
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                         gap_event_cb, NULL);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGE(TAG, "adv_start rc=%d", rc);
  }
}

static void on_sync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc == 0) {
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  }
  if (rc != 0) {
    ESP_LOGE(TAG, "pas d'adresse BLE utilisable (rc=%d)", rc);
    return;
  }
  start_advertising();
  ESP_LOGI(TAG, "prêt pour le jumelage (%s)", BLE_DEVICE_NAME);
}

static void on_reset(int reason) {
  ESP_LOGW(TAG, "réinitialisation de l'hôte NimBLE (raison %d)", reason);
}

static void host_task(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

/* Scanne le Wi-Fi tant qu'un central est connecté et publie la liste sur la
 * caractéristique `networks` : c'est elle que pog Console affiche pour que
 * l'humain choisisse un réseau que l'enceinte voit vraiment. */
static void networks_task(void *arg) {
  (void)arg;
  for (;;) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    publish_networks(true, NULL, 0);
    wifi_ap_record_t *records = NULL;
    uint16_t count = 0;
    if (wifi_scan_keep_connected(&records, &count) == ESP_OK) {
      publish_networks(false, records, count);
      free(records);
    } else {
      publish_networks(false, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(15000));
  }
}

esp_err_t ble_onboarding_start(void) {
  s_json_lock = xSemaphoreCreateMutex();
  if (s_json_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }
  strlcpy(s_status_json, "{\"status\":\"ready\"}", sizeof(s_status_json));
  strlcpy(s_networks_json, "{\"scanning\":true,\"networks\":[]}",
          sizeof(s_networks_json));

  const esp_timer_create_args_t timer_args = {
      .callback = restart_timer_cb,
      .name = "ble_onboard_restart",
  };
  esp_err_t err = esp_timer_create(&timer_args, &s_restart_timer);
  if (err != ESP_OK) {
    return err;
  }

  err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
    return err;
  }

  ble_hs_cfg.reset_cb = on_reset;
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  /* Jumelage « Just Works » avec bonding et Secure Connections : pas d'écran
   * ni de clavier sur l'enceinte, mais un lien chiffré et des clés conservées
   * pour les reconnexions. Le même choix que Jarvis. */
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_our_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist =
      BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(s_gatt_svcs);
  if (rc == 0) {
    rc = ble_gatts_add_svcs(s_gatt_svcs);
  }
  if (rc != 0) {
    ESP_LOGE(TAG, "enregistrement GATT rc=%d", rc);
    return ESP_FAIL;
  }
  ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
  ble_store_config_init();

  nimble_port_freertos_init(host_task);

  if (xTaskCreate(networks_task, "ble_networks", 4096, NULL, 3, NULL) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "onboarding BLE annoncé — service POG, nom %s",
           BLE_DEVICE_NAME);
  return ESP_OK;
}
