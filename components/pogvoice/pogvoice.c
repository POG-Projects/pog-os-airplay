#include "pogvoice.h"
#include "sdkconfig.h"

#ifndef CONFIG_POG_VOICE
pogvoice_light_t pogvoice_light_state(void) {
  return POGVOICE_LIGHT_OFF;
}
bool pogvoice_paired(void) {
  return false;
}
void pogvoice_init(pogvoice_capture_start_fn cb) {
  (void)cb;
}
cJSON *pogvoice_status(void) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "supported", false);
  return j;
}
esp_err_t pogvoice_enroll(const char *u, const char *c, bool l) {
  (void)u;
  (void)c;
  (void)l;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pogvoice_forget(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pogvoice_start(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t pogvoice_start_wake(const char *phrase) {
  (void)phrase;
  return ESP_ERR_NOT_SUPPORTED;
}
void pogvoice_finish(void) {
}
void pogvoice_cancel(void) {
}
bool pogvoice_busy(void) {
  return false;
}
bool pogvoice_speaker_read(int16_t *p, size_t c, size_t *n) {
  (void)p;
  (void)c;
  (void)n;
  return false;
}
#else
#include "pogvoice_protocol.h"
#include "pogvoice_resample.h"
#include "pogvoice_playout.h"
#include "pogvoice_worker.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPEAKER_SAMPLES   32768
#define SPEAKER_PREBUFFER (CONFIG_OUTPUT_SAMPLE_RATE_HZ * 240 / 1000)
/* About 740 ms at 44.1 kHz mono PCM16, in PSRAM, to absorb Wi-Fi jitter. */
#define MIC_BYTES         65536
#define UPLINK_FRAME_MS   60
#define UPLINK_SAMPLES    (16000 * UPLINK_FRAME_MS / 1000)
typedef struct {
  uint32_t version;
  char api[201], token[129], client[33];
  bool lan;
} voice_config_t;
typedef struct {
  uint8_t opcode;
  size_t size;
  uint8_t bytes[];
} message_t;
typedef struct {
  esp_websocket_client_handle_t ws;
  QueueHandle_t messages;
  StreamBufferHandle_t mic;
  StaticStreamBuffer_t *mic_control;
  uint8_t *mic_storage;
  pogvoice_message_t assembly;
  uint8_t opus[1275], packet[1279];
  uint32_t encode_us, encode_max_us, send_us, send_max_us, frames;
  uint32_t decode_us, decode_max_us, decoded_frames;
  uint32_t resample_us, resample_max_us, resample_cpu_us, resample_cpu_max_us;
  uint32_t reply_gap_max_us, reply_late_gaps;
  int64_t last_reply_us;
  atomic_bool connected, disconnected, failed, mic_done;
  atomic_int mic_error;
} connection_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static voice_config_t s_config;
static pogvoice_capture_start_fn s_capture;
static char s_device[18], s_state[24] = "unpaired", s_error[128], s_text[384];
static char s_wake_phrase[24];
static bool s_busy, s_pairing, s_finish, s_cancel;
static int16_t *s_speaker;
static size_t s_head, s_tail, s_count;
static uint32_t s_uploaded, s_downloaded;
static pogvoice_playout_t s_playout;

pogvoice_light_t pogvoice_light_state(void) {
  portENTER_CRITICAL(&s_lock);
  pogvoice_light_t light = POGVOICE_LIGHT_OFF;
  if (!s_cancel) {
    if (!strcmp(s_state, "connecting") || !strcmp(s_state, "pairing"))
      light = POGVOICE_LIGHT_CONNECTING;
    else if (!strcmp(s_state, "listening"))
      light = POGVOICE_LIGHT_LISTENING;
    else if (!strcmp(s_state, "thinking"))
      light = POGVOICE_LIGHT_THINKING;
    else if (!strcmp(s_state, "speaking"))
      light = POGVOICE_LIGHT_SPEAKING;
    else if (!strcmp(s_state, "error"))
      light = POGVOICE_LIGHT_ERROR;
  }
  portEXIT_CRITICAL(&s_lock);
  return light;
}

bool pogvoice_paired(void) {
  portENTER_CRITICAL(&s_lock);
  bool paired = s_config.token[0] != 0;
  portEXIT_CRITICAL(&s_lock);
  return paired;
}

static void state(const char *value) {
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_state, value, sizeof(s_state));
  portEXIT_CRITICAL(&s_lock);
}
static bool cancelled(void) {
  portENTER_CRITICAL(&s_lock);
  bool v = s_cancel;
  portEXIT_CRITICAL(&s_lock);
  return v;
}
static bool finish_requested(void) {
  portENTER_CRITICAL(&s_lock);
  bool v = s_finish;
  portEXIT_CRITICAL(&s_lock);
  return v;
}
static void result_error(const char *value) {
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_error, value, sizeof(s_error));
  portEXIT_CRITICAL(&s_lock);
}
static const char *str(const cJSON *j, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
static int number(const cJSON *j, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsNumber(v) ? v->valueint : 0;
}

void pogvoice_init(pogvoice_capture_start_fn capture) {
  s_capture = capture;
  esp_err_t worker_error = pogvoice_worker_init();
  if (worker_error != ESP_OK)
    result_error("Mémoire interne insuffisante pour la voix");
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_device, sizeof(s_device), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  nvs_handle_t n;
  if (nvs_open("pogvoice", NVS_READONLY, &n) == ESP_OK) {
    size_t len = sizeof(s_config);
    if (nvs_get_blob(n, "config", &s_config, &len) != ESP_OK ||
        len != sizeof(s_config) || s_config.version != 1 ||
        !memchr(s_config.api, 0, sizeof(s_config.api)) ||
        !memchr(s_config.token, 0, sizeof(s_config.token)) ||
        !memchr(s_config.client, 0, sizeof(s_config.client)))
      memset(&s_config, 0, sizeof(s_config));
    nvs_close(n);
  }
  s_speaker = heap_caps_calloc(SPEAKER_SAMPLES, sizeof(int16_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_speaker)
    result_error("Mémoire PSRAM insuffisante pour les réponses");
  state(s_config.token[0] ? "ready" : "unpaired");
}

cJSON *pogvoice_status(void) {
  char api[201], current[24], error[128], text[384];
  portENTER_CRITICAL(&s_lock);
  strlcpy(api, s_config.api, sizeof(api));
  strlcpy(current, s_state, sizeof(current));
  strlcpy(error, s_error, sizeof(error));
  strlcpy(text, s_text, sizeof(text));
  bool paired = s_config.token[0] != 0, busy = s_busy, lan = s_config.lan;
  uint32_t up = s_uploaded, down = s_downloaded;
  pogvoice_playout_t playout = s_playout;
  portEXIT_CRITICAL(&s_lock);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "supported", true);
  cJSON_AddBoolToObject(j, "paired", paired);
  cJSON_AddBoolToObject(j, "busy", busy);
  cJSON_AddBoolToObject(j, "allow_insecure_lan", lan);
  cJSON_AddStringToObject(j, "api_url", api);
  cJSON_AddStringToObject(j, "device_id", s_device);
  cJSON_AddStringToObject(j, "state", current);
  cJSON_AddStringToObject(j, "error", error);
  cJSON_AddStringToObject(j, "text", text);
  cJSON_AddNumberToObject(j, "uploaded_frames", up);
  cJSON_AddNumberToObject(j, "downloaded_frames", down);
  cJSON_AddNumberToObject(j, "playout_underruns", playout.underruns);
  cJSON_AddNumberToObject(j, "played_samples", playout.played_samples);
  cJSON_AddNumberToObject(j, "max_listen_seconds", 15);
  cJSON_AddBoolToObject(j, "aec", false);
  cJSON_AddBoolToObject(j, "wake_word", false);
  return j;
}

/* Cleartext requires explicit opt-in and a private IPv4 destination. The
 * operator pins the host; bootstrap may change its port, never its host/TLS. */
static bool endpoint_valid(const char *url, bool websocket, bool lan) {
  pogvoice_origin_t origin;
  if (!pogvoice_origin_parse(url, websocket, &origin))
    return false;
  if (origin.secure)
    return true;
  if (!lan)
    return false;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM},
                  *a = NULL;
  if (getaddrinfo(origin.host, NULL, &hints, &a) != 0 || !a)
    return false;
  bool ok = true;
  for (struct addrinfo *p = a; p; p = p->ai_next) {
    uint32_t ip = ntohl(((struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr);
    if (!((ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 ||
          (ip >> 16) == 0xa9fe))
      ok = false;
  }
  freeaddrinfo(a);
  return ok;
}

static cJSON *http_json(const voice_config_t *cfg, const char *path,
                        const char *body) {
  if (!endpoint_valid(cfg->api, false, cfg->lan))
    return NULL;
  char url[300];
  size_t n = strlen(cfg->api);
  snprintf(url, sizeof(url), "%.*s%s",
           (int)(n && cfg->api[n - 1] == '/' ? n - 1 : n), cfg->api, path);
  esp_http_client_config_t options = {.url = url,
                                      .timeout_ms = 6000,
                                      .crt_bundle_attach =
                                          esp_crt_bundle_attach,
                                      .disable_auto_redirect = true};
  esp_http_client_handle_t h = esp_http_client_init(&options);
  if (!h)
    return NULL;
  esp_http_client_set_method(h, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
  esp_http_client_set_header(h, "Device-Id", s_device);
  esp_http_client_set_header(h, "Client-Id", cfg->client);
  esp_http_client_set_header(h, "User-Agent", "POG-AirPlay-Voice/1");
  esp_http_client_set_header(h, "Content-Type", "application/json");
  char response[4097];
  size_t used = 0;
  cJSON *json = NULL;
  size_t length = body ? strlen(body) : 0;
  if (esp_http_client_open(h, (int)length) != ESP_OK)
    goto done;
  if (length && esp_http_client_write(h, body, (int)length) != (int)length)
    goto done;
  if (esp_http_client_fetch_headers(h) < 0)
    goto done;
  int status = esp_http_client_get_status_code(h);
  if (status < 200 || status >= 300)
    goto done;
  while (used < sizeof(response)) {
    int got = esp_http_client_read(h, response + used,
                                   (int)(sizeof(response) - used));
    if (got < 0)
      goto done;
    if (got == 0)
      break;
    used += (size_t)got;
  }
  if (used == sizeof(response) || !esp_http_client_is_complete_data_received(h))
    goto done;
  response[used] = 0;
  json = cJSON_ParseWithLength(response, used + 1);
done:
  mbedtls_platform_zeroize(response, sizeof(response));
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  return json;
}

typedef struct {
  char api[201], code[33];
  bool lan;
} enrol_job_t;

static void enroll_task(void *arg) {
  enrol_job_t *job = arg;
  const char *api = job->api, *code = job->code;
  bool lan = job->lan;
  voice_config_t cfg = {.version = 1, .lan = lan};
  strlcpy(cfg.api, api, sizeof(cfg.api));
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); i++)
    snprintf(cfg.client + 2 * i, 3, "%02x", random[i]);
  cJSON *request = cJSON_CreateObject();
  cJSON_AddStringToObject(request, "device_id", s_device);
  cJSON_AddStringToObject(request, "name", "POG AirPlay");
  cJSON_AddStringToObject(request, "kind", "speaker");
  cJSON_AddStringToObject(request, "code", code);
  char *body = cJSON_PrintUnformatted(request);
  cJSON_Delete(request);
  cJSON *reply = body ? http_json(&cfg, "/api/v1/enroll/claim", body) : NULL;
  if (body) {
    mbedtls_platform_zeroize(body, strlen(body));
    cJSON_free(body);
  }
  esp_err_t err = ESP_FAIL;
  bool claimed = false;
  const char *token = str(reply, "token");
  if (strlen(token) >= 16 && strlen(token) < sizeof(cfg.token) &&
      !strchr(token, '\r') && !strchr(token, '\n') &&
      !strcmp(str(reply, "device_id"), s_device)) {
    strlcpy(cfg.token, token, sizeof(cfg.token));
    claimed = true;
    nvs_handle_t n;
    err = nvs_open("pogvoice", NVS_READWRITE, &n);
    if (err == ESP_OK) {
      err = nvs_set_blob(n, "config", &cfg, sizeof(cfg));
      if (err == ESP_OK)
        err = nvs_commit(n);
      nvs_close(n);
    }
    if (err == ESP_OK) {
      portENTER_CRITICAL(&s_lock);
      s_config = cfg;
      s_error[0] = 0;
      portEXIT_CRITICAL(&s_lock);
    }
  }
  cJSON_Delete(reply);
  mbedtls_platform_zeroize(&cfg, sizeof(cfg));
  mbedtls_platform_zeroize(job, sizeof(*job));
  free(job);
  portENTER_CRITICAL(&s_lock);
  s_pairing = false;
  strlcpy(s_state, s_config.token[0] ? "ready" : "unpaired", sizeof(s_state));
  if (err != ESP_OK)
    strlcpy(s_error,
            claimed
                ? "Jeton reçu mais non sauvegardé : recommencer avec un "
                  "nouveau code"
                : "Association refusée : vérifier le serveur et le code POG AI",
            sizeof(s_error));
  portEXIT_CRITICAL(&s_lock);
  /* Return to the reusable audio worker. */
}

esp_err_t pogvoice_enroll(const char *api, const char *code, bool lan) {
  pogvoice_origin_t origin;
  if (!api || !code || strlen(api) > 200 || strlen(code) < 6 ||
      strlen(code) > 32 || !pogvoice_origin_parse(api, false, &origin) ||
      (!origin.secure && !lan))
    return ESP_ERR_INVALID_ARG;
  enrol_job_t *job = calloc(1, sizeof(*job));
  if (!job)
    return ESP_ERR_NO_MEM;
  strlcpy(job->api, api, sizeof(job->api));
  strlcpy(job->code, code, sizeof(job->code));
  job->lan = lan;
  portENTER_CRITICAL(&s_lock);
  if (s_busy || s_pairing) {
    portEXIT_CRITICAL(&s_lock);
    mbedtls_platform_zeroize(job, sizeof(*job));
    free(job);
    return ESP_ERR_INVALID_STATE;
  }
  s_pairing = true;
  s_error[0] = 0;
  strlcpy(s_state, "pairing", sizeof(s_state));
  portEXIT_CRITICAL(&s_lock);
  esp_err_t queued = pogvoice_worker_submit(enroll_task, job, 3);
  if (queued != ESP_OK) {
    mbedtls_platform_zeroize(job, sizeof(*job));
    free(job);
    portENTER_CRITICAL(&s_lock);
    s_pairing = false;
    strlcpy(s_state, "error", sizeof(s_state));
    portEXIT_CRITICAL(&s_lock);
    return queued;
  }
  return ESP_OK;
}

esp_err_t pogvoice_forget(void) {
  portENTER_CRITICAL(&s_lock);
  if (s_busy || s_pairing) {
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_pairing = true;
  portEXIT_CRITICAL(&s_lock);
  nvs_handle_t n;
  esp_err_t err = nvs_open("pogvoice", NVS_READWRITE, &n);
  if (err == ESP_OK) {
    err = nvs_erase_key(n, "config");
    if (err == ESP_ERR_NVS_NOT_FOUND)
      err = ESP_OK;
    if (err == ESP_OK)
      err = nvs_commit(n);
    nvs_close(n);
  }
  portENTER_CRITICAL(&s_lock);
  if (err == ESP_OK) {
    memset(&s_config, 0, sizeof(s_config));
    s_error[0] = s_text[0] = 0;
    strlcpy(s_state, "unpaired", sizeof(s_state));
  }
  s_pairing = false;
  portEXIT_CRITICAL(&s_lock);
  return err;
}

bool pogvoice_busy(void) {
  portENTER_CRITICAL(&s_lock);
  bool b = s_busy || s_pairing;
  portEXIT_CRITICAL(&s_lock);
  return b;
}
void pogvoice_finish(void) {
  portENTER_CRITICAL(&s_lock);
  s_finish = true;
  portEXIT_CRITICAL(&s_lock);
}
void pogvoice_cancel(void) {
  portENTER_CRITICAL(&s_lock);
  bool active = s_busy;
  s_cancel = true;
  s_count = s_head = s_tail = 0;
  portEXIT_CRITICAL(&s_lock);
  if (active)
    pogmic_stream_stop();
}
bool pogvoice_speaker_read(int16_t *pcm, size_t cap, size_t *frames) {
  portENTER_CRITICAL(&s_lock);
  bool active = s_busy;
  size_t consumed = 0;
  if (active) {
    consumed =
        pogvoice_playout_take(&s_playout, s_count, cap, SPEAKER_PREBUFFER);
    for (size_t i = 0; i < consumed; i++) {
      pcm[2 * i] = pcm[2 * i + 1] = s_speaker[s_tail];
      s_tail = (s_tail + 1) % SPEAKER_SAMPLES;
    }
    s_count -= consumed;
  }
  portEXIT_CRITICAL(&s_lock);
  if (active) {
    /* Keep writes paced by I2S even while waiting for speech. A short or
     * empty reply queue must not slow the draining of live AirPlay music. */
    memset(pcm + consumed * 2, 0, (cap - consumed) * 2 * sizeof(*pcm));
    *frames = cap;
  }
  return active;
}

static bool speaker_write(const int16_t *pcm, size_t n) {
  int64_t deadline = esp_timer_get_time() + 2000000;
  while (n && !cancelled()) {
    portENTER_CRITICAL(&s_lock);
    size_t count = SPEAKER_SAMPLES - s_count;
    if (count > n)
      count = n;
    for (size_t i = 0; i < count; i++) {
      s_speaker[s_head] = pcm[i];
      s_head = (s_head + 1) % SPEAKER_SAMPLES;
    }
    s_count += count;
    portEXIT_CRITICAL(&s_lock);
    pcm += count;
    n -= count;
    if (n) {
      if (esp_timer_get_time() > deadline)
        return false;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  return !n;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t event,
                     void *data) {
  (void)base;
  connection_t *c = arg;
  if (event == WEBSOCKET_EVENT_CONNECTED) {
    atomic_store(&c->connected, true);
    return;
  }
  if (event == WEBSOCKET_EVENT_DISCONNECTED ||
      event == WEBSOCKET_EVENT_CLOSED || event == WEBSOCKET_EVENT_FINISH) {
    atomic_store(&c->disconnected, true);
    return;
  }
  if (event != WEBSOCKET_EVENT_DATA)
    return;
  esp_websocket_event_data_t *e = data;
  if (e->payload_offset < 0 || e->payload_len < 0 || e->data_len < 0) {
    atomic_store(&c->failed, true);
    return;
  }
  int result =
      pogvoice_message_feed(&c->assembly, e->op_code, e->fin, e->payload_offset,
                            e->payload_len, e->data_ptr, e->data_len);
  if (result < 0)
    atomic_store(&c->failed, true);
  if (result != 1)
    return;
  if (c->assembly.opcode == 2 && c->assembly.used >= 4 &&
      c->assembly.bytes[0] == 0) {
    int64_t now = esp_timer_get_time();
    if (c->last_reply_us) {
      uint32_t gap = (uint32_t)(now - c->last_reply_us);
      if (gap > c->reply_gap_max_us)
        c->reply_gap_max_us = gap;
      if (gap > 100000)
        c->reply_late_gaps++;
    }
    c->last_reply_us = now;
  }
  message_t *m = heap_caps_malloc(sizeof(*m) + c->assembly.used + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) {
    atomic_store(&c->failed, true);
    return;
  }
  m->opcode = c->assembly.opcode;
  m->size = c->assembly.used;
  memcpy(m->bytes, c->assembly.bytes, m->size + 1);
  /* Bounded callback backpressure lets TCP slow fast TTS producers. */
  if (xQueueSend(c->messages, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
    free(m);
    atomic_store(&c->failed, true);
  }
}

static bool send_json(connection_t *c, cJSON *j) {
  char *text = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (!text)
    return false;
  size_t n = strlen(text);
  int sent =
      esp_websocket_client_send_text(c->ws, text, (int)n, pdMS_TO_TICKS(1000));
  cJSON_free(text);
  return sent == (int)n;
}
static bool control(connection_t *c, const char *type, const char *value,
                    const char *session) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "type", type);
  cJSON_AddStringToObject(j, !strcmp(type, "abort") ? "reason" : "state",
                          value);
  cJSON_AddStringToObject(j, "session_id", session);
  if (!strcmp(type, "listen")) {
    cJSON_AddStringToObject(j, "mode", s_wake_phrase[0] ? "auto" : "manual");
    if (!strcmp(value, "detect"))
      cJSON_AddStringToObject(j, "text", s_wake_phrase);
  }
  return send_json(c, j);
}
static esp_err_t mic_data(const int16_t *pcm, size_t n, uint32_t rate,
                          esp_err_t error, void *arg) {
  connection_t *c = arg;
  if (!pcm) {
    atomic_store(&c->mic_error, error);
    atomic_store(&c->mic_done, true);
    return ESP_OK;
  }
  if (rate != CONFIG_OUTPUT_SAMPLE_RATE_HZ)
    return ESP_ERR_INVALID_STATE;
  if (cancelled()) {
    pogmic_stream_stop();
    return ESP_OK;
  }
  size_t bytes = n * sizeof(*pcm);
  if (xStreamBufferSend(c->mic, pcm, bytes, 0) == bytes)
    return ESP_OK;
  ESP_LOGW("pogvoice", "mic backlog exceeded: queued=%u bytes",
           (unsigned)xStreamBufferBytesAvailable(c->mic));
  return ESP_ERR_TIMEOUT;
}

static bool encode_frame(connection_t *c, void *encoder, int16_t *pcm) {
  esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm,
                                 .len = UPLINK_SAMPLES * sizeof(*pcm)};
  esp_audio_enc_out_frame_t out = {.buffer = c->opus, .len = sizeof(c->opus)};
  if (!s_uploaded)
    ESP_LOGI("pogvoice", "first encode: core=%d stack_min=%u", xPortGetCoreID(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  int64_t started = esp_timer_get_time();
  if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK)
    return false;
  uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
  c->encode_us += elapsed;
  if (elapsed > c->encode_max_us)
    c->encode_max_us = elapsed;
  size_t n = pogvoice_v3_encode(c->packet, sizeof(c->packet), c->opus,
                                out.encoded_bytes);
  started = esp_timer_get_time();
  if (!n || esp_websocket_client_send_bin(c->ws, (char *)c->packet, (int)n,
                                          pdMS_TO_TICKS(250)) != (int)n)
    return false;
  elapsed = (uint32_t)(esp_timer_get_time() - started);
  c->send_us += elapsed;
  if (elapsed > c->send_max_us)
    c->send_max_us = elapsed;
  c->frames++;
  if (c->frames % 100 == 0)
    ESP_LOGI("pogvoice",
             "upload frames=%u encode_us=%u/%u send_us=%u/%u queued=%u",
             (unsigned)c->frames, (unsigned)(c->encode_us / c->frames),
             (unsigned)c->encode_max_us, (unsigned)(c->send_us / c->frames),
             (unsigned)c->send_max_us,
             (unsigned)xStreamBufferBytesAvailable(c->mic));
  portENTER_CRITICAL(&s_lock);
  s_uploaded++;
  portEXIT_CRITICAL(&s_lock);
  return true;
}

static void conversation(void *unused) {
  (void)unused;
  voice_config_t cfg;
  portENTER_CRITICAL(&s_lock);
  cfg = s_config;
  portEXIT_CRITICAL(&s_lock);
  connection_t *c =
      heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  /* PCM scratch is CPU-only, not DMA. Keep it out of the internal heap
   * needed by I2S, Wi-Fi and task control blocks. */
  pogvoice_resampler_t *up =
      heap_caps_calloc(1, sizeof(*up), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  pogvoice_resampler_t *down =
      heap_caps_calloc(1, sizeof(*down), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void *encoder = NULL, *decoder = NULL;
  int16_t *decoded = heap_caps_malloc(2880 * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  int16_t *converted = heap_caps_malloc(1024 * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  unsigned reply_rate = 0;
  bool mic_started = false, listening = false, hello_sent = false, tts = false,
       done = false;
  char session[65] = "", headers[400], ws_url[201] = "";
  const char *failure = "Mémoire insuffisante";
  if (!c || !up || !down || !decoded || !converted)
    goto cleanup;
  /* Keep the kernel's control structures and spinlocks internal. Only the
   * microphone payload needs PSRAM; the dynamic stream-buffer allocator would
   * otherwise spend scarce DMA-capable RAM on the whole payload. */
  c->messages = xQueueCreate(128, sizeof(message_t *));
  c->mic_control = heap_caps_calloc(1, sizeof(*c->mic_control),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  c->mic_storage =
      heap_caps_malloc(MIC_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (c->mic_control && c->mic_storage)
    c->mic = xStreamBufferCreateStatic(MIC_BYTES + 1, 1, c->mic_storage,
                                       c->mic_control);
  if (!c->messages || !c->mic)
    goto cleanup;
  cJSON *bootstrap = http_json(&cfg, "/xiaozhi/ota/", NULL);
  const cJSON *ws = cJSON_GetObjectItemCaseSensitive(bootstrap, "websocket");
  strlcpy(ws_url, str(ws, "url"), sizeof(ws_url));
  bool endpoint = number(ws, "version") == 3 &&
                  pogvoice_ws_origin_allowed(cfg.api, ws_url, cfg.lan) &&
                  endpoint_valid(ws_url, true, cfg.lan);
  cJSON_Delete(bootstrap);
  if (!endpoint) {
    failure =
        "Bootstrap POG AI indisponible ou adresse WebSocket non autorisée";
    goto cleanup;
  }
  snprintf(headers, sizeof(headers),
           "Device-Id: %s\r\nClient-Id: %s\r\nProtocol-Version: "
           "3\r\nAuthorization: Bearer %s\r\n",
           s_device, cfg.client, cfg.token);
  esp_websocket_client_config_t options = {.uri = ws_url,
                                           .headers = headers,
                                           .crt_bundle_attach =
                                               esp_crt_bundle_attach,
                                           .disable_auto_reconnect = true,
                                           .network_timeout_ms = 5000,
                                           .task_stack = 6144,
                                           .buffer_size = 2048,
                                           .ping_interval_sec = 10,
                                           .pingpong_timeout_sec = 30};
  c->ws = esp_websocket_client_init(&options);
  if (!c->ws)
    goto cleanup;
  esp_websocket_register_events(c->ws, WEBSOCKET_EVENT_ANY, ws_event, c);
  if (esp_websocket_client_start(c->ws) != ESP_OK) {
    failure = "Connexion POG AI impossible";
    goto cleanup;
  }
  int64_t deadline = esp_timer_get_time() + 12000000;
  int16_t frame[UPLINK_SAMPLES];
  size_t frame_used = 0;
  failure = NULL;
  while (!done && !cancelled()) {
    if (atomic_load(&c->failed) || atomic_load(&c->disconnected)) {
      failure = "Connexion POG AI interrompue ou jeton refusé";
      break;
    }
    if (esp_timer_get_time() > deadline) {
      failure = "Délai POG AI dépassé";
      break;
    }
    if (atomic_load(&c->connected) && !hello_sent) {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "type", "hello");
      cJSON_AddNumberToObject(j, "version", 3);
      cJSON_AddStringToObject(j, "transport", "websocket");
      cJSON *f = cJSON_AddObjectToObject(j, "features");
      cJSON_AddBoolToObject(f, "aec", false);
      cJSON_AddBoolToObject(f, "mcp", false);
      cJSON *a = cJSON_AddObjectToObject(j, "audio_params");
      cJSON_AddStringToObject(a, "format", "opus");
      cJSON_AddNumberToObject(a, "sample_rate", 16000);
      cJSON_AddNumberToObject(a, "channels", 1);
      cJSON_AddNumberToObject(a, "frame_duration", UPLINK_FRAME_MS);
      if (!send_json(c, j)) {
        failure = "Envoi du hello impossible";
        break;
      }
      hello_sent = true;
    }
    bool worked = false;
    message_t *m = NULL;
    if (xQueueReceive(c->messages, &m, 0) == pdTRUE) {
      worked = true;
      const uint8_t *payload = m->bytes;
      size_t length = m->size;
      uint8_t type = 1;
      bool valid =
          m->opcode == 1 ||
          (m->opcode == 2 &&
           pogvoice_v3_decode(m->bytes, m->size, &type, &payload, &length));
      if (!valid)
        failure = "Trame POG AI invalide";
      else if (type == 0) {
        if (!decoder || !tts || listening || length == 0 || length > 1275)
          failure = "Audio POG AI inattendu";
        else {
          esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)payload,
                                        .len = length};
          esp_audio_dec_out_frame_t pcm = {.buffer = (uint8_t *)decoded,
                                           .len = 2880 * sizeof(int16_t)};
          esp_audio_dec_info_t info = {0};
          int64_t decode_started = esp_timer_get_time();
          if (esp_opus_dec_decode(decoder, &raw, &pcm, &info) !=
                  ESP_AUDIO_ERR_OK ||
              raw.consumed != length || pcm.decoded_size % 2 ||
              pcm.decoded_size > 2880 * sizeof(int16_t) ||
              info.sample_rate != reply_rate || info.channel != 1 ||
              info.bits_per_sample != 16)
            failure = "Décodage Opus impossible";
          else {
            uint32_t decode_us =
                (uint32_t)(esp_timer_get_time() - decode_started);
            c->decode_us += decode_us;
            if (decode_us > c->decode_max_us)
              c->decode_max_us = decode_us;
            c->decoded_frames++;
            int64_t resample_started = esp_timer_get_time();
            size_t samples = pcm.decoded_size / 2;
            for (size_t i = 0; i < samples && !failure; i += 160) {
              size_t n = samples - i;
              if (n > 160)
                n = 160;
              int64_t cpu_started = esp_timer_get_time();
              int got = pogvoice_resampler_process(down, decoded + i, n,
                                                   converted, 1024);
              uint32_t cpu_us = (uint32_t)(esp_timer_get_time() - cpu_started);
              c->resample_cpu_us += cpu_us;
              if (cpu_us > c->resample_cpu_max_us)
                c->resample_cpu_max_us = cpu_us;
              if (got < 0 || !speaker_write(converted, (size_t)got))
                failure = "Sortie audio indisponible";
            }
            uint32_t resample_us =
                (uint32_t)(esp_timer_get_time() - resample_started);
            c->resample_us += resample_us;
            if (resample_us > c->resample_max_us)
              c->resample_max_us = resample_us;
            portENTER_CRITICAL(&s_lock);
            s_downloaded++;
            portEXIT_CRITICAL(&s_lock);
          }
        }
      } else {
        cJSON *j = cJSON_ParseWithLength((const char *)payload, length);
        const char *kind = str(j, "type");
        if (!j)
          failure = "Message POG AI invalide";
        else if (!strcmp(kind, "hello")) {
          ESP_LOGI("pogvoice", "hello received: DMA free=%u largest=%u",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
          const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "audio_params");
          int rate = number(a, "sample_rate"), ms = number(a, "frame_duration");
          if (session[0] || !*str(j, "session_id") ||
              strlen(str(j, "session_id")) >= sizeof(session) ||
              strcmp(str(j, "transport"), "websocket") ||
              strcmp(str(a, "format"), "opus") || number(a, "channels") != 1 ||
              !(rate == 8000 || rate == 12000 || rate == 16000 ||
                rate == 24000 || rate == 48000) ||
              !(ms == 10 || ms == 20 || ms == 40 || ms == 60))
            failure = "Format audio POG AI non pris en charge";
          else {
            strlcpy(session, str(j, "session_id"), sizeof(session));
            esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
            enc.sample_rate = 16000;
            enc.channel = 1;
            enc.bits_per_sample = 16;
            enc.bitrate = 24000;
            enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
            /* CELT costs less CPU on this target. Pack 60 ms per send to
             * amortize network scheduling, keeping standard Opus framing.
             * Reserve 40 KB stack; heap metrics exclude scratch stack. */
            enc.application_mode = ESP_OPUS_ENC_APPLICATION_LOWDELAY;
            enc.complexity = 0;
            reply_rate = (unsigned)rate;
            esp_opus_dec_cfg_t dec = {
                .sample_rate = rate,
                .channel = 1,
                .frame_duration = ms == 10   ? ESP_OPUS_DEC_FRAME_DURATION_10_MS
                                  : ms == 20 ? ESP_OPUS_DEC_FRAME_DURATION_20_MS
                                  : ms == 40
                                      ? ESP_OPUS_DEC_FRAME_DURATION_40_MS
                                      : ESP_OPUS_DEC_FRAME_DURATION_60_MS,
                .self_delimited = false};
            if (esp_opus_enc_open(&enc, sizeof(enc), &encoder) !=
                    ESP_AUDIO_ERR_OK ||
                esp_opus_dec_open(&dec, sizeof(dec), &decoder) !=
                    ESP_AUDIO_ERR_OK ||
                pogvoice_resampler_init(up, CONFIG_OUTPUT_SAMPLE_RATE_HZ,
                                        16000) != 0 ||
                pogvoice_resampler_init(down, rate,
                                        CONFIG_OUTPUT_SAMPLE_RATE_HZ) != 0)
              failure = "Initialisation des codecs impossible";
            else if (finish_requested()) {
              done = true;
            } else if (!control(c, "listen",
                                s_wake_phrase[0] ? "detect" : "start", session))
              failure = "Démarrage de l’écoute impossible";
            else {
              esp_err_t capture_error = s_capture(mic_data, c);
              ESP_LOGI(
                  "pogvoice", "capture start: %s DMA free=%u largest=%u",
                  esp_err_to_name(capture_error),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
              if (capture_error != ESP_OK)
                failure = capture_error == ESP_ERR_NO_MEM
                              ? "Mémoire interne insuffisante pour le micro"
                              : "Micro indisponible ou GPIO déjà utilisés";
              else {
                mic_started = listening = true;
                state("listening");
                deadline = esp_timer_get_time() + 75000000;
              }
            }
          }
        } else if (!strcmp(kind, "tts")) {
          const char *v = str(j, "state");
          if (!session[0] || !mic_started)
            failure = "Réponse reçue avant une demande";
          else if (!strcmp(v, "start")) {
            /* POG AI may detect the end of speech before the manual limit.
             * Stop capture before admitting speaker audio: no AEC is present.
             * The gateway already ended this utterance; sending another
             * listen-stop here could trigger inference a second time. */
            if (listening) {
              pogmic_stream_stop();
              while (!atomic_load(&c->mic_done))
                vTaskDelay(pdMS_TO_TICKS(10));
              xStreamBufferReset(c->mic);
              frame_used = 0;
              listening = false;
              ESP_LOGI("pogvoice", "server reply: microphone stopped");
            }
            tts = true;
            state("speaking");
            deadline = esp_timer_get_time() + 60000000;
          } else if (listening) {
            failure = "Audio reçu avant le début de la réponse";
          } else if (!strcmp(v, "stop")) {
            portENTER_CRITICAL(&s_lock);
            s_playout.finished = true;
            portEXIT_CRITICAL(&s_lock);
            done = true;
          }
        } else if ((!strcmp(kind, "system") || !strcmp(kind, "System")) &&
                   !strcmp(str(j, "command"), "abort")) {
          /* Another room won wake arbitration. Do not keep uploading or play
           * a stale response while the selected satellite handles the turn. */
          pogvoice_cancel();
          done = true;
        } else if (!strcmp(kind, "alert")) {
          result_error(str(j, "message"));
          failure = "POG AI a refusé la demande";
        }
        if (!strcmp(kind, "stt") || !strcmp(kind, "llm") ||
            (!strcmp(kind, "tts") && *str(j, "text"))) {
          portENTER_CRITICAL(&s_lock);
          strlcpy(s_text, str(j, "text"), sizeof(s_text));
          portEXIT_CRITICAL(&s_lock);
        }
        cJSON_Delete(j);
      }
      free(m);
      if (failure)
        break;
    }
    if (listening) {
      if (finish_requested())
        pogmic_stream_stop();
      int16_t chunk[160];
      size_t bytes = xStreamBufferReceive(c->mic, chunk, sizeof(chunk), 0);
      if (bytes) {
        worked = true;
        int got =
            pogvoice_resampler_process(up, chunk, bytes / 2, converted, 1024);
        if (got < 0) {
          failure = "Conversion micro impossible";
          break;
        }
        for (int i = 0; i < got; i++) {
          frame[frame_used++] = converted[i];
          if (frame_used == UPLINK_SAMPLES) {
            if (!encode_frame(c, encoder, frame)) {
              failure = "Envoi du micro interrompu";
              break;
            }
            frame_used = 0;
          }
        }
        if (failure)
          break;
      }
      if (atomic_load(&c->mic_done) &&
          xStreamBufferBytesAvailable(c->mic) == 0) {
        esp_err_t mic_error = atomic_load(&c->mic_error);
        if (mic_error != ESP_OK) {
          ESP_LOGW("pogvoice", "capture failed: %s, DMA free=%u largest=%u",
                   esp_err_to_name(mic_error),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
          failure = mic_error == ESP_ERR_NO_MEM
                        ? "Mémoire interne insuffisante pour le micro"
                        : "Capture micro interrompue";
          break;
        }
        if (frame_used) {
          memset(frame + frame_used, 0,
                 (UPLINK_SAMPLES - frame_used) * sizeof(int16_t));
          if (!encode_frame(c, encoder, frame)) {
            failure = "Dernière trame micro non envoyée";
            break;
          }
        }
        if (!control(c, "listen", "stop", session)) {
          failure = "Fin d’écoute non envoyée";
          break;
        }
        listening = false;
        state("thinking");
        deadline = esp_timer_get_time() + 60000000;
      }
    }
    /* At 100 Hz a 2 ms delay truncates to zero ticks and starves idle.
     * Drain buffered mic chunks without sleeping, then block a whole tick. */
    if (!worked)
      vTaskDelay(1);
  }
  if (done && !failure && !cancelled()) {
    /* Keep ownership through the final DMA tail before music resumes. */
    int64_t drain = esp_timer_get_time() + 3000000;
    for (;;) {
      portENTER_CRITICAL(&s_lock);
      size_t pending = s_count;
      portEXIT_CRITICAL(&s_lock);
      if (!pending || cancelled())
        break;
      if (esp_timer_get_time() > drain) {
        failure = "Lecture de la réponse bloquée";
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
cleanup:
  if (mic_started) {
    pogmic_stream_stop();
    /* Never free the callback context while the I2S task still owns it. */
    while (!atomic_load(&c->mic_done))
      vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (c && c->ws) {
    if (session[0] && (failure || cancelled()))
      control(c, "abort", "cancelled", session);
    esp_websocket_client_stop(c->ws);
    esp_websocket_client_destroy(c->ws);
  }
  if (c && c->messages) {
    message_t *m;
    while (xQueueReceive(c->messages, &m, 0) == pdTRUE)
      free(m);
    vQueueDelete(c->messages);
  }
  if (c && c->mic)
    vStreamBufferDelete(c->mic);
  if (c) {
    free(c->mic_control);
    free(c->mic_storage);
  }
  if (encoder)
    esp_opus_enc_close(encoder);
  if (decoder)
    esp_opus_dec_close(decoder);
  pogvoice_resampler_free(up);
  pogvoice_resampler_free(down);
  free(up);
  free(down);
  free(decoded);
  free(converted);
  if (c && c->frames)
    ESP_LOGI("pogvoice",
             "upload complete frames=%u encode_us=%u/%u send_us=%u/%u",
             (unsigned)c->frames, (unsigned)(c->encode_us / c->frames),
             (unsigned)c->encode_max_us, (unsigned)(c->send_us / c->frames),
             (unsigned)c->send_max_us);
  if (c && c->decoded_frames)
    ESP_LOGI("pogvoice",
             "reply frames=%u decode_us=%u/%u played=%u underruns=%u",
             (unsigned)c->decoded_frames,
             (unsigned)(c->decode_us / c->decoded_frames),
             (unsigned)c->decode_max_us, (unsigned)s_playout.played_samples,
             (unsigned)s_playout.underruns);
  if (c && c->decoded_frames)
    ESP_LOGI("pogvoice",
             "reply timing: resample_cpu_us=%u/%u resample_write_us=%u/%u "
             "network_gap_max_us=%u gaps_over_100ms=%u",
             (unsigned)(c->resample_cpu_us / c->decoded_frames),
             (unsigned)c->resample_cpu_max_us,
             (unsigned)(c->resample_us / c->decoded_frames),
             (unsigned)c->resample_max_us, (unsigned)c->reply_gap_max_us,
             (unsigned)c->reply_late_gaps);
  free(c);
  mbedtls_platform_zeroize(&cfg, sizeof(cfg));
  mbedtls_platform_zeroize(headers, sizeof(headers));
  portENTER_CRITICAL(&s_lock);
  if (failure && !s_cancel)
    strlcpy(s_error, failure, sizeof(s_error));
  strlcpy(s_state, failure && !s_cancel ? "error" : "ready", sizeof(s_state));
  s_busy = false;
  s_count = s_head = s_tail = 0;
  portEXIT_CRITICAL(&s_lock);
  ESP_LOGI("pogvoice", "turn complete: DMA free=%u largest=%u stack_min=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  /* Return to the reusable audio worker. */
}

esp_err_t pogvoice_start_wake(const char *phrase) {
  pogmic_status_t mic;
  pogmic_get_status(&mic);
  if (!s_capture || !s_speaker || !mic.configured || !mic.clock_ready ||
      mic.active)
    return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&s_lock);
  if (s_busy || s_pairing || !s_config.token[0]) {
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_busy = true;
  s_finish = s_cancel = false;
  s_count = s_head = s_tail = 0;
  s_uploaded = s_downloaded = 0;
  s_playout = (pogvoice_playout_t){0};
  s_error[0] = s_text[0] = 0;
  strlcpy(s_wake_phrase, phrase ? phrase : "", sizeof(s_wake_phrase));
  strlcpy(s_state, "connecting", sizeof(s_state));
  portEXIT_CRITICAL(&s_lock);
  ESP_LOGI(
      "pogvoice", "start: reserved_stack=%u internal free=%u largest=%u",
      (unsigned)POGVOICE_WORKER_STACK_BYTES,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT));
  /* AirPlay decode/playback are pinned to core 1. Explicitly keep voice on
   * core 0 instead of letting the first floating-point operation pin it to
   * whichever core happened to run it, behind the priority-8 music decoder. */
  esp_err_t queued = pogvoice_worker_submit(conversation, NULL, 4);
  if (queued != ESP_OK) {
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    strlcpy(s_state, "error", sizeof(s_state));
    strlcpy(s_error,
            queued == ESP_ERR_NO_MEM ? "Mémoire insuffisante"
                                     : "Traitement audio occupé",
            sizeof(s_error));
    portEXIT_CRITICAL(&s_lock);
    return queued;
  }
  return ESP_OK;
}
esp_err_t pogvoice_start(void) {
  return pogvoice_start_wake(NULL);
}
#endif
