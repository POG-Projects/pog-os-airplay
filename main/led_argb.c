#include "led_argb.h"

#include "settings.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "led_argb";

// WS2812 over the ESP-IDF led_strip RMT backend.
#define ARGB_RES_HZ     (10 * 1000 * 1000)
#define ARGB_FPS        50
#define ARGB_FRAME_US   (1000000 / ARGB_FPS)
#define ARGB_TASK_STACK 4096
#define ARGB_TASK_PRIO  2
#define ARGB_TASK_CORE  0
#define ARGB_MAX_LEDS   300

// ============================================================================
// Shared audio-analysis state (single producer = audio task, single consumer =
// render task). Identical analysis to the LED matrix.
// ============================================================================

typedef struct {
  float level;  // overall RMS level, ~0..1 (smoothed + AGC)
  float bass;   // low-frequency energy, ~0..1
  float treble; // high-frequency energy, ~0..1
} argb_audio_t;

static portMUX_TYPE s_audio_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile argb_audio_t s_audio = {0};
static uint32_t s_audio_feed_calls;
static float s_peak = 1000.0f; // AGC peak tracker (audio task only)

// ============================================================================
// Live config + runtime state
// ============================================================================

typedef struct {
  int fx;
  int brightness;
  int speed;
  uint8_t cr;
  uint8_t cg;
  uint8_t cb;
  uint32_t revision;
} argb_config_t;

/* Web and pogdev handlers update the requested configuration from other
 * FreeRTOS tasks.  The render task takes one coherent snapshot per frame;
 * without this hand-off the compiler is allowed to retain the old effect,
 * brightness or colour indefinitely. */
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;
static argb_config_t s_config = {
    .fx = 0,
    .brightness = 128,
    .speed = 5,
    .cr = 0x20,
    .cg = 0x80,
    .cb = 0xFF,
    .revision = 0,
};
/* Renderer-owned copy.  Effect helpers only read this copy. */
static argb_config_t s_frame_config;

static volatile bool s_enabled = false; // fast-path gate for led_argb_feed()
static bool s_running = false;
static int s_gpio = -1;
static int s_count = 0;

/* Au bout de ce nombre de trames sans acquittement, on cesse de journaliser et
 * on considère la sortie muette : le pilote RMT sort une erreur par trame, soit
 * ~50 par seconde, ce qui rend la console inexploitable. */
#define ARGB_TX_FAILURES_MAX 20
static int s_tx_failures = 0;

static led_strip_handle_t s_strip = NULL;
static uint8_t *s_fb = NULL;  // working RGB framebuffer (count*3: R,G,B)
static uint8_t *s_aux = NULL; // per-LED scratch (twinkle decay), count bytes

static TaskHandle_t s_task = NULL;
static volatile bool s_task_stop = false;

// ============================================================================
// Colour helpers
// ============================================================================

// h,s,v in 0..255 -> r,g,b in 0..255 (cheap integer-ish HSV).
static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g,
                    uint8_t *b) {
  uint8_t region = h / 43;
  uint8_t rem = (h - region * 43) * 6;
  uint8_t p = (uint8_t)((v * (255 - s)) / 255);
  uint8_t q = (uint8_t)((v * (255 - ((s * rem) / 255))) / 255);
  uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) / 255))) / 255);
  switch (region) {
  case 0:
    *r = v;
    *g = t;
    *b = p;
    break;
  case 1:
    *r = q;
    *g = v;
    *b = p;
    break;
  case 2:
    *r = p;
    *g = v;
    *b = t;
    break;
  case 3:
    *r = p;
    *g = q;
    *b = v;
    break;
  case 4:
    *r = t;
    *g = p;
    *b = v;
    break;
  default:
    *r = v;
    *g = p;
    *b = q;
    break;
  }
}

static inline void fb_set(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= s_count) {
    return;
  }
  s_fb[i * 3 + 0] = r;
  s_fb[i * 3 + 1] = g;
  s_fb[i * 3 + 2] = b;
}

// Set LED i to the user base colour scaled by intensity v (0..255).
static inline void fb_set_color(int i, uint8_t v) {
  fb_set(i, (uint8_t)(s_frame_config.cr * v / 255),
         (uint8_t)(s_frame_config.cg * v / 255),
         (uint8_t)(s_frame_config.cb * v / 255));
}

// ============================================================================
// Effects: render into s_fb (R,G,B per LED), full 0..255 — the master
// brightness is applied later in strip_show().
// ============================================================================

// --- 0: VU-metre (bar grows from the start, green->red, white peak dot) -----
static void fx_vu(const argb_audio_t *a) {
  static float bar = 0.0f, peak = 0.0f;
  int n = s_count;
  float target = a->level * (float)n;
  if (target > bar) {
    bar = target;
  } else {
    bar -= (float)n * 0.04f;
    if (bar < target)
      bar = target;
    if (bar < 0.0f)
      bar = 0.0f;
  }
  if (bar > peak) {
    peak = bar;
  } else {
    peak -= (float)n * 0.012f;
    if (peak < bar)
      peak = bar;
  }
  int lit = (int)(bar + 0.5f);
  if (lit > n)
    lit = n;
  int pk = (int)(peak + 0.5f);
  if (pk > n)
    pk = n;
  for (int i = 0; i < n; i++) {
    if (i < lit) {
      float frac = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
      uint8_t hue = (uint8_t)(96.0f * (1.0f - frac)); // green -> red
      uint8_t r, g, b;
      hsv2rgb(hue, 255, 255, &r, &g, &b);
      fb_set(i, r, g, b);
    } else {
      fb_set(i, 0, 0, 0);
    }
  }
  if (pk > 0 && pk <= n) {
    fb_set(pk - 1, 255, 255, 255);
  }
}

// --- 1: Spectre (bass at start -> treble at end) ----------------------------
static void fx_spectrum(const argb_audio_t *a) {
  int n = s_count;
  for (int i = 0; i < n; i++) {
    float w = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
    float band = a->bass * (1.0f - w) + a->treble * w;
    if (band < 0.0f)
      band = 0.0f;
    if (band > 1.0f)
      band = 1.0f;
    uint8_t hue = (uint8_t)(170.0f * w);
    uint8_t r, g, b;
    hsv2rgb(hue, 255, (uint8_t)(band * 255.0f), &r, &g, &b);
    fb_set(i, r, g, b);
  }
}

// --- 2: Pulsation basses (whole strip pulses in the base colour) ------------
static void fx_bass_pulse(const argb_audio_t *a) {
  static float pulse = 0.0f;
  float target = a->bass;
  if (target > pulse) {
    pulse = target;
  } else {
    pulse -= 0.05f;
    if (pulse < target)
      pulse = target;
    if (pulse < 0.0f)
      pulse = 0.0f;
  }
  uint8_t v = (uint8_t)(pulse * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 3: Arc-en-ciel (flowing rainbow, speed-controlled) ---------------------
static void fx_rainbow(void) {
  static uint32_t t = 0;
  t += (uint32_t)s_frame_config.speed;
  int n = s_count;
  for (int i = 0; i < n; i++) {
    uint8_t hue =
        (uint8_t)(((uint32_t)i * 256u / (uint32_t)(n > 0 ? n : 1) + t) & 0xFF);
    uint8_t r, g, b;
    hsv2rgb(hue, 255, 255, &r, &g, &b);
    fb_set(i, r, g, b);
  }
}

// --- 4: Veilleuse (warm white breathing, speed-controlled) ------------------
static void fx_nightlight(void) {
  static float t = 0.0f;
  t += 0.01f * (float)s_frame_config.speed;
  float breathe = 0.35f + 0.35f * sinf(t * 0.5f);
  uint8_t v = (uint8_t)(breathe * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set(i, v, (uint8_t)(v * 0.6f), (uint8_t)(v * 0.18f));
  }
}

// --- 5: VU centre (bar grows from the middle outwards) ----------------------
static void fx_vu_center(const argb_audio_t *a) {
  static float bar = 0.0f;
  int n = s_count;
  int half = n / 2;
  float target = a->level * (float)(half > 0 ? half : 1);
  if (target > bar) {
    bar = target;
  } else {
    bar -= (float)n * 0.025f;
    if (bar < target)
      bar = target;
    if (bar < 0.0f)
      bar = 0.0f;
  }
  int lit = (int)(bar + 0.5f);
  for (int i = 0; i < n; i++) {
    int dist = (i < half) ? (half - 1 - i) : (i - half);
    if (dist < lit) {
      float frac = (half > 0) ? (float)dist / (float)half : 0.0f;
      uint8_t hue =
          (uint8_t)(96.0f * (1.0f - frac)); // green centre -> red edge
      uint8_t r, g, b;
      hsv2rgb(hue, 255, 255, &r, &g, &b);
      fb_set(i, r, g, b);
    } else {
      fb_set(i, 0, 0, 0);
    }
  }
}

// --- 6: Strobe basses (the strip flashes the base colour on each beat) ------
static void fx_beat_strobe(const argb_audio_t *a) {
  static float flash = 0.0f, prev = 0.0f;
  if (a->bass > prev + 0.13f && a->bass > 0.35f) {
    flash = 1.0f; // rising bass transient => trigger
  }
  prev = a->bass;
  flash -= 0.03f * (float)s_frame_config.speed;
  if (flash < 0.0f)
    flash = 0.0f;
  uint8_t v = (uint8_t)(flash * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 7: Couleur fixe (solid base colour) ------------------------------------
static void fx_solid(void) {
  for (int i = 0; i < s_count; i++) {
    fb_set(i, s_frame_config.cr, s_frame_config.cg, s_frame_config.cb);
  }
}

// --- 8: Respiration (base colour breathing, speed-controlled) ---------------
static void fx_breathe(void) {
  static float t = 0.0f;
  t += 0.012f * (float)s_frame_config.speed;
  float b = 0.12f + 0.88f * (0.5f + 0.5f * sinf(t));
  uint8_t v = (uint8_t)(b * 255.0f);
  for (int i = 0; i < s_count; i++) {
    fb_set_color(i, v);
  }
}

// --- 9: Comete / scanner (base colour dot sweeping with a fading tail) ------
static void fx_comet(void) {
  static float pos = 0.0f;
  static int dir = 1;
  int n = s_count;
  pos += (float)dir * (0.12f * (float)s_frame_config.speed);
  if (pos >= (float)(n - 1)) {
    pos = (float)(n - 1);
    dir = -1;
  } else if (pos <= 0.0f) {
    pos = 0.0f;
    dir = 1;
  }
  int head = (int)(pos + 0.5f);
  for (int i = 0; i < n; i++) {
    int d = head - i;
    if (d < 0)
      d = -d;
    float fade = 1.0f - (float)d / 5.0f; // ~5-pixel tail
    if (fade < 0.0f)
      fade = 0.0f;
    fb_set_color(i, (uint8_t)(fade * 255.0f));
  }
}

// --- 10: Scintillement (random sparkles in the base colour) -----------------
static void fx_twinkle(void) {
  int n = s_count;
  int spawns = 1 + s_frame_config.speed / 3;
  for (int k = 0; k < spawns; k++) {
    if ((esp_random() & 0xFF) < 100) {
      s_aux[esp_random() % (uint32_t)n] = 255;
    }
  }
  int dec = 5 + s_frame_config.speed * 2;
  for (int i = 0; i < n; i++) {
    uint8_t v = s_aux[i];
    fb_set_color(i, v);
    s_aux[i] = (v > dec) ? (uint8_t)(v - dec) : 0;
  }
}

// --- 11: Niveau couleur (whole strip hue mapped from the loudness) ----------
static void fx_level_color(const argb_audio_t *a) {
  static float lv = 0.0f;
  float t = a->level;
  lv = (t > lv) ? t : (lv * 0.85f + t * 0.15f);
  uint8_t hue = (uint8_t)(160.0f * (1.0f - lv)); // blue (quiet) -> red (loud)
  uint8_t v = (uint8_t)(50.0f + 205.0f * lv);
  uint8_t r, g, b;
  hsv2rgb(hue, 255, v, &r, &g, &b);
  for (int i = 0; i < s_count; i++) {
    fb_set(i, r, g, b);
  }
}

// ============================================================================
// Strip output: apply master brightness, then let the ESP-IDF strip encoder
// produce the WS2812 GRB stream and reset/latch symbol over RMT.
// ============================================================================

static void strip_show(void) {
  /* Sortie considérée muette : on ne retente plus jusqu'à la prochaine
   * reconfiguration afin qu'un défaut RMT ne noie pas la console à 50 Hz. */
  if (s_tx_failures >= ARGB_TX_FAILURES_MAX) {
    return;
  }

  int br = s_frame_config.brightness;
  if (br < 0)
    br = 0;
  if (br > 255)
    br = 255;
  esp_err_t err = ESP_OK;
  for (int i = 0; i < s_count && err == ESP_OK; i++) {
    uint8_t r = s_fb[i * 3 + 0];
    uint8_t g = s_fb[i * 3 + 1];
    uint8_t b = s_fb[i * 3 + 2];
    err = led_strip_set_pixel(s_strip, (uint32_t)i, (uint8_t)((r * br) / 255),
                              (uint8_t)((g * br) / 255),
                              (uint8_t)((b * br) / 255));
  }
  if (err == ESP_OK) {
    err = led_strip_refresh(s_strip);
  }
  if (err == ESP_OK) {
    s_tx_failures = 0;
  } else if (s_tx_failures < ARGB_TX_FAILURES_MAX) {
    if (++s_tx_failures == ARGB_TX_FAILURES_MAX) {
      ESP_LOGE(TAG,
               "sortie ARGB en échec sur GPIO%d après %d trames — rendu "
               "suspendu: %s",
               s_gpio, ARGB_TX_FAILURES_MAX, esp_err_to_name(err));
    }
  }
}

// ============================================================================
// Render task
// ============================================================================

static void render_task(void *arg) {
  (void)arg;
  int64_t next_us = esp_timer_get_time();

  while (!s_task_stop) {
    argb_config_t config;
    portENTER_CRITICAL(&s_config_mux);
    config = s_config;
    portEXIT_CRITICAL(&s_config_mux);
    if (config.revision != s_frame_config.revision) {
      s_frame_config = config;
      /* A live change must also wake an output suspended after RMT failures;
       * previously only a reboot or a GPIO/count change cleared this state. */
      s_tx_failures = 0;
    }

    argb_audio_t a;
    portENTER_CRITICAL(&s_audio_mux);
    a.level = s_audio.level;
    a.bass = s_audio.bass;
    a.treble = s_audio.treble;
    portEXIT_CRITICAL(&s_audio_mux);

    memset(s_fb, 0, (size_t)s_count * 3);
    switch (s_frame_config.fx) {
    case 1:
      fx_spectrum(&a);
      break;
    case 2:
      fx_bass_pulse(&a);
      break;
    case 3:
      fx_rainbow();
      break;
    case 4:
      fx_nightlight();
      break;
    case 5:
      fx_vu_center(&a);
      break;
    case 6:
      fx_beat_strobe(&a);
      break;
    case 7:
      fx_solid();
      break;
    case 8:
      fx_breathe();
      break;
    case 9:
      fx_comet();
      break;
    case 10:
      fx_twinkle();
      break;
    case 11:
      fx_level_color(&a);
      break;
    case 0:
    default:
      fx_vu(&a);
      break;
    }
    strip_show();

    next_us += ARGB_FRAME_US;
    int64_t now = esp_timer_get_time();
    int64_t delay_us = next_us - now;
    if (delay_us < 1000) {
      next_us = now;
      vTaskDelay(1);
    } else {
      TickType_t ticks = pdMS_TO_TICKS(delay_us / 1000);
      if (ticks == 0)
        ticks = 1;
      vTaskDelay(ticks);
    }
  }

  memset(s_fb, 0, (size_t)s_count * 3);
  strip_show();

  s_task = NULL;
  vTaskDelete(NULL);
}

// ============================================================================
// Start / stop
// ============================================================================

static void argb_free_strip(void) {
  s_tx_failures = 0;
  if (s_strip) {
    led_strip_del(s_strip);
    s_strip = NULL;
  }
}

static void argb_free_buffers(void) {
  free(s_fb);
  s_fb = NULL;
  free(s_aux);
  s_aux = NULL;
}

static void argb_stop(void) {
  if (!s_running) {
    return;
  }
  s_enabled = false;

  if (s_task) {
    s_task_stop = true;
    for (int i = 0; i < 200 && s_task != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (s_task != NULL) {
      // The render task didn't exit within the timeout (e.g. starved under
      // load). Freeing its frame/output buffers now would be a use-after-free
      // the moment it resumes, so leave them allocated (a safe leak) and keep
      // s_task_stop set so the task exits on its own. Do NOT clear s_task_stop
      // here.
      ESP_LOGE(
          TAG,
          "strip render task did not stop in time; buffers left allocated");
      return;
    }
  }
  s_task_stop = false;

  argb_free_strip();
  argb_free_buffers();
  s_running = false;
  ESP_LOGI(TAG, "strip stopped");
}

static void apply_config(int fx, int brightness, uint32_t color, int speed) {
  portENTER_CRITICAL(&s_config_mux);
  s_config.fx = fx;
  s_config.brightness = brightness;
  s_config.cr = (uint8_t)((color >> 16) & 0xFF);
  s_config.cg = (uint8_t)((color >> 8) & 0xFF);
  s_config.cb = (uint8_t)(color & 0xFF);
  s_config.speed = (speed < 1) ? 1 : (speed > 10 ? 10 : speed);
  s_config.revision++;
  portEXIT_CRITICAL(&s_config_mux);
}

static void argb_start(int fx, int brightness, int gpio, int count,
                       uint32_t color, int speed) {
  if (gpio < 0 || count <= 0) {
    ESP_LOGW(TAG, "strip enabled but invalid (gpio=%d count=%d)", gpio, count);
    return;
  }
  if (count > ARGB_MAX_LEDS) {
    count = ARGB_MAX_LEDS;
  }

  s_fb = malloc((size_t)count * 3);
  s_aux = calloc((size_t)count, 1);
  if (!s_fb || !s_aux) {
    ESP_LOGE(TAG, "strip buffer alloc failed");
    argb_free_buffers();
    return;
  }
  memset(s_fb, 0, (size_t)count * 3);

  led_strip_config_t strip_cfg = {
      .strip_gpio_num = gpio,
      .max_leds = (uint32_t)count,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags = {.invert_out = false},
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .mem_block_symbols = 64,
      .resolution_hz = ARGB_RES_HZ,
      .flags = {.with_dma = false},
  };
  esp_err_t strip_err =
      led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
  if (strip_err != ESP_OK) {
    ESP_LOGE(TAG, "led strip init failed (gpio=%d): %s", gpio,
             esp_err_to_name(strip_err));
    argb_free_buffers();
    return;
  }

  s_gpio = gpio;
  s_count = count;
  apply_config(fx, brightness, color, speed);

  portENTER_CRITICAL(&s_audio_mux);
  s_audio.level = 0.0f;
  s_audio.bass = 0.0f;
  s_audio.treble = 0.0f;
  s_audio_feed_calls = 0;
  portEXIT_CRITICAL(&s_audio_mux);
  s_peak = 1000.0f;

  s_task_stop = false;
  s_running = true;
  s_enabled = true;

  BaseType_t ok =
      xTaskCreatePinnedToCore(render_task, "led_argb", ARGB_TASK_STACK, NULL,
                              ARGB_TASK_PRIO, &s_task, ARGB_TASK_CORE);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "failed to create render task");
    s_enabled = false;
    s_running = false;
    s_task = NULL;
    argb_free_strip();
    argb_free_buffers();
    return;
  }

  ESP_LOGI(TAG, "strip started: fx=%d br=%d gpio=%d count=%d speed=%d", fx,
           brightness, gpio, count, speed);
}

// ============================================================================
// Public API
// ============================================================================

void led_argb_init(void) {
  bool en = false;
  int fx = 0, br = 128, gpio = -1, count = 0, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);
  if (!en) {
    ESP_LOGI(TAG, "strip disabled (no RMT, no task)");
    return;
  }
  argb_start(fx, br, gpio, count, color, speed);
}

void led_argb_reconfigure(void) {
  bool en = false;
  int fx = 0, br = 128, gpio = -1, count = 0, speed = 5;
  uint32_t color = 0x2080FF;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);

  if (!en) {
    argb_stop();
    return;
  }

  // Same GPIO + count already running: apply fx/brightness/colour/speed live.
  if (s_running && gpio == s_gpio && count == s_count) {
    apply_config(fx, br, color, speed);
    ESP_LOGI(TAG, "strip updated live: fx=%d br=%d speed=%d", fx, br, speed);
    return;
  }

  argb_stop();
  argb_start(fx, br, gpio, count, color, speed);
}

void led_argb_feed(const int16_t *pcm, size_t stereo_samples) {
  if (!s_enabled || stereo_samples == 0 || pcm == NULL) {
    return;
  }

  size_t total = stereo_samples * 2;
  uint64_t sum_sq = 0, bass_sum = 0, diff_sum = 0;
  int32_t prev = pcm[0];
  for (size_t i = 0; i < total; i++) {
    int32_t s = pcm[i];
    sum_sq += (uint64_t)(s * s);
    int32_t as = s < 0 ? -s : s;
    bass_sum += (uint64_t)as;
    int32_t d = s - prev;
    diff_sum += (uint64_t)(d < 0 ? -d : d);
    prev = s;
  }

  float rms = sqrtf((float)sum_sq / (float)total);
  float bass_abs = (float)bass_sum / (float)total;
  float treble_abs = (float)diff_sum / (float)total;

  if (rms > s_peak) {
    s_peak = rms;
  } else {
    s_peak *= 0.999f;
  }
  if (s_peak < 500.0f) {
    s_peak = 500.0f;
  }

  float level = rms / s_peak;
  float bass = (bass_abs * 1.4f) / s_peak;
  float treble = (treble_abs * 0.9f) / s_peak;
  if (level > 1.0f)
    level = 1.0f;
  if (bass > 1.0f)
    bass = 1.0f;
  if (treble > 1.0f)
    treble = 1.0f;

  portENTER_CRITICAL(&s_audio_mux);
  float pl = s_audio.level, pb = s_audio.bass, pt = s_audio.treble;
  s_audio.level = (level > pl) ? level : (pl * 0.8f + level * 0.2f);
  s_audio.bass = (bass > pb) ? bass : (pb * 0.85f + bass * 0.15f);
  s_audio.treble = (treble > pt) ? treble : (pt * 0.7f + treble * 0.3f);
  s_audio_feed_calls++;
  portEXIT_CRITICAL(&s_audio_mux);
}

void led_argb_get_audio_stats(led_argb_audio_stats_t *stats) {
  if (!stats) {
    return;
  }
  portENTER_CRITICAL(&s_audio_mux);
  stats->feed_calls = s_audio_feed_calls;
  stats->level = s_audio.level;
  stats->bass = s_audio.bass;
  stats->treble = s_audio.treble;
  portEXIT_CRITICAL(&s_audio_mux);
}
