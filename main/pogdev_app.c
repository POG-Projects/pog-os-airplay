#include "pogdev_app.h"

#include "amp_ctrl.h"
#include "eq_dsp.h"
#include "effect_sync.h"
#include "led_argb.h"
#include "playback_control.h"
#include "pogdev_bus.h"
#include "settings.h"
#include "web_server.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pogdev_app";
static bool s_argb_effect_capable;

static bool argb_follow_capable(void) {
  bool enabled = false;
  int gpio = -1, count = 0;
  settings_get_argb(&enabled, &gpio, &count, NULL, NULL, NULL, NULL, NULL);
  return s_argb_effect_capable || (enabled && gpio >= 0 && count > 0);
}

/* ================================================================== */
/*  Construction du descripteur                                        */
/* ================================================================== */

/* La catégorie est la section dans laquelle pog Home range l'entité sur la
 * carte de l'appareil. Elle ne se devine pas : `led` et `tone` portent toutes
 * deux `on_off`, `bass` et `led_brightness` toutes deux `number` — un
 * regroupement dérivé des traits mettrait les graves et la luminosité de la LED
 * côte à côte. Seul le firmware sait que `bass` appartient à l'égaliseur.
 *
 * Le vocabulaire est ouvert : une catégorie n'est jamais comparée qu'aux autres
 * catégories du MÊME appareil, donc deux firmwares qui n'emploient pas les
 * mêmes mots ne cassent rien. Ceux d'ici sont ceux du protocole (pogdev.md
 * §4.3). */
static cJSON *add_entity(cJSON *arr, const char *key, const char *name,
                         const char *category) {
  cJSON *e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "key", key);
  cJSON_AddStringToObject(e, "name", name);
  cJSON_AddStringToObject(e, "category", category);
  cJSON_AddItemToObject(e, "traits", cJSON_CreateArray());
  cJSON_AddItemToArray(arr, e);
  return e;
}

/* Renvoie l'objet `config` du trait ajouté, pour que l'appelant y pose son
 * schéma : bornes, options, lecture seule. */
static cJSON *add_trait(cJSON *entity, const char *id) {
  cJSON *t = cJSON_CreateObject();
  cJSON_AddStringToObject(t, "id", id);
  cJSON *cfg = cJSON_AddObjectToObject(t, "config");
  cJSON_AddItemToArray(cJSON_GetObjectItem(entity, "traits"), t);
  return cfg;
}

static void add_number(cJSON *arr, const char *key, const char *name,
                       const char *category, double min, double max,
                       double step, const char *unit) {
  cJSON *cfg = add_trait(add_entity(arr, key, name, category), "number");
  cJSON_AddNumberToObject(cfg, "min", min);
  cJSON_AddNumberToObject(cfg, "max", max);
  cJSON_AddNumberToObject(cfg, "step", step);
  cJSON_AddStringToObject(cfg, "unit", unit);
  cJSON_AddStringToObject(cfg, "label", name);
}

/* Les options d'un choix : un identifiant stable côté machine, un libellé
 * français côté humain. Renommer un libellé ne doit pas casser une scène qui
 * enregistrait le choix. */
typedef struct {
  const char *value;
  const char *label;
} option_t;

static void add_select(cJSON *arr, const char *key, const char *name,
                       const char *category, const option_t *opts, int n) {
  cJSON *cfg = add_trait(add_entity(arr, key, name, category), "select");
  cJSON_AddStringToObject(cfg, "label", name);
  cJSON *list = cJSON_AddArrayToObject(cfg, "options");
  for (int i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "value", opts[i].value);
    cJSON_AddStringToObject(o, "label", opts[i].label);
    cJSON_AddItemToArray(list, o);
  }
}

static void add_command(cJSON *cfg_commands, const char *name,
                        const char *label) {
  cJSON *c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "name", name);
  cJSON_AddStringToObject(c, "label", label);
  /* Toutes réversibles : appuyer sur « Suivant » se rattrape avec
   * « Précédent ». Sans ce drapeau, le serveur les classerait sensibles et
   * exigerait le niveau le plus élevé pour changer de morceau. */
  cJSON_AddBoolToObject(c, "reversible", true);
  cJSON_AddItemToArray(cfg_commands, c);
}

static const option_t CHANNELS[] = {{"stereo", "Stéréo"},
                                    {"mono", "Mono"},
                                    {"left", "Gauche"},
                                    {"right", "Droite"}};

/* L'ordre est celui des effets dans led_argb.c : l'index EST la valeur stockée
 * en NVS, il ne peut pas être réordonné sans changer ce qui est persisté. */
static const option_t ARGB_EFFECTS[] = {
    {"0", "VU-mètre"},      {"1", "Spectre"},        {"2", "Pulsation basses"},
    {"3", "Arc-en-ciel"},   {"4", "Veilleuse"},      {"5", "VU centre"},
    {"6", "Strobe basses"}, {"7", "Couleur fixe"},   {"8", "Respiration"},
    {"9", "Comète"},        {"10", "Scintillement"}, {"11", "Niveau couleur"}};

/* Sous-ensemble portable d'abord, puis les effets qui exigent l'analyseur
 * audio de l'enceinte. Une ambiance multi-appareils prend l'intersection des
 * listes annoncées ; une enceinte seule garde donc toute sa richesse. */
static const char *SYNC_EFFECTS[] = {
    "Uni",          "Arc-en-ciel",       "Respiration",
    "Comète",       "Scintillement",     "Veilleuse",
    "VU-mètre",     "Spectre",           "Pulsation basses",
    "VU centre",    "Strobe basses",     "Niveau couleur",
};

static void add_light_effect_action(cJSON *lamp) {
  cJSON *cfg = add_trait(lamp, "action");
  cJSON *commands = cJSON_AddArrayToObject(cfg, "commands");
  cJSON *command = cJSON_CreateObject();
  cJSON_AddStringToObject(command, "name", "sync_effect");
  cJSON_AddStringToObject(command, "label", "Synchroniser l’ambiance");
  cJSON_AddBoolToObject(command, "sensitive", false);
  cJSON_AddBoolToObject(command, "reversible", true);
  cJSON *params = cJSON_AddArrayToObject(command, "params");
  cJSON *effect = cJSON_CreateObject();
  cJSON_AddStringToObject(effect, "name", "effect");
  cJSON_AddStringToObject(effect, "kind", "enum");
  cJSON_AddBoolToObject(effect, "required", true);
  cJSON *options = cJSON_AddArrayToObject(effect, "enum");
  for (size_t i = 0; i < sizeof(SYNC_EFFECTS) / sizeof(SYNC_EFFECTS[0]); i++) {
    cJSON_AddItemToArray(options, cJSON_CreateString(SYNC_EFFECTS[i]));
  }
  cJSON_AddItemToArray(params, effect);
  static const struct {
    const char *name;
    double min;
    double max;
  } numbers[] = {{"speed", 0, 100},
                 {"brightness", 0, 100},
                 {"primary_hue", 0, 360},
                 {"primary_saturation", 0, 100},
                 {"secondary_hue", 0, 360},
                 {"secondary_saturation", 0, 100}};
  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
    cJSON *param = cJSON_CreateObject();
    cJSON_AddStringToObject(param, "name", numbers[i].name);
    cJSON_AddStringToObject(param, "kind", "number");
    cJSON_AddBoolToObject(param, "required", true);
    cJSON_AddNumberToObject(param, "min", numbers[i].min);
    cJSON_AddNumberToObject(param, "max", numbers[i].max);
    cJSON_AddItemToArray(params, param);
  }
  cJSON_AddItemToArray(commands, command);
  if (!argb_follow_capable()) return;

  cJSON *join = cJSON_CreateObject();
  cJSON_AddStringToObject(join, "name", "join_effect_group");
  cJSON_AddStringToObject(join, "label", "Suivre une ambiance musicale");
  cJSON_AddBoolToObject(join, "sensitive", false);
  cJSON_AddBoolToObject(join, "reversible", true);
  cJSON *join_params = cJSON_AddArrayToObject(join, "params");
#define ADD_STRING_PARAM(name_value) do { \
  cJSON *p = cJSON_CreateObject(); \
  cJSON_AddStringToObject(p, "name", name_value); \
  cJSON_AddStringToObject(p, "kind", "string"); \
  cJSON_AddBoolToObject(p, "required", true); \
  cJSON_AddItemToArray(join_params, p); \
} while (0)
  ADD_STRING_PARAM("group_id");
  cJSON *role = cJSON_CreateObject();
  cJSON_AddStringToObject(role, "name", "role");
  cJSON_AddStringToObject(role, "kind", "enum");
  cJSON_AddBoolToObject(role, "required", true);
  cJSON *roles = cJSON_AddArrayToObject(role, "enum");
  cJSON_AddItemToArray(roles, cJSON_CreateString("leader"));
  cJSON_AddItemToArray(roles, cJSON_CreateString("follower"));
  cJSON_AddItemToArray(join_params, role);
  ADD_STRING_PARAM("leader_entity_id");
#undef ADD_STRING_PARAM
  const struct { const char *name; int min; int max; } timing[] = {
      {"presentation_delay_ms", 0, 500},
      {"calibration_offset_ms", -100, 100},
  };
  for (size_t i = 0; i < 2; ++i) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", timing[i].name);
    cJSON_AddStringToObject(p, "kind", "number");
    cJSON_AddBoolToObject(p, "required", true);
    cJSON_AddNumberToObject(p, "min", timing[i].min);
    cJSON_AddNumberToObject(p, "max", timing[i].max);
    cJSON_AddItemToArray(join_params, p);
  }
  cJSON *visualizer = cJSON_CreateObject();
  cJSON_AddStringToObject(visualizer, "name", "visualizer");
  cJSON_AddStringToObject(visualizer, "kind", "enum");
  cJSON_AddBoolToObject(visualizer, "required", false);
  cJSON_AddStringToObject(visualizer, "default", "spectrum");
  cJSON *visualizers = cJSON_AddArrayToObject(visualizer, "enum");
  static const char *visualizer_names[] = {
      "spectrum", "vu_meter", "bass_pulse", "rainbow"};
  for (size_t i = 0; i < 4; ++i)
    cJSON_AddItemToArray(visualizers,
                         cJSON_CreateString(visualizer_names[i]));
  cJSON_AddItemToArray(join_params, visualizer);
  cJSON_AddItemToArray(commands, join);

  cJSON *leave = cJSON_CreateObject();
  cJSON_AddStringToObject(leave, "name", "leave_effect_group");
  cJSON_AddStringToObject(leave, "label", "Quitter l’ambiance musicale");
  cJSON_AddBoolToObject(leave, "sensitive", false);
  cJSON_AddBoolToObject(leave, "reversible", true);
  cJSON *leave_params = cJSON_AddArrayToObject(leave, "params");
  cJSON *group = cJSON_CreateObject();
  cJSON_AddStringToObject(group, "name", "group_id");
  cJSON_AddStringToObject(group, "kind", "string");
  cJSON_AddBoolToObject(group, "required", true);
  cJSON_AddItemToArray(leave_params, group);
  cJSON_AddItemToArray(commands, leave);
}

static cJSON *add_modes(cJSON *cfg, const char *mode) {
  cJSON *modes = cJSON_AddArrayToObject(cfg, "modes");
  cJSON_AddItemToArray(modes, cJSON_CreateString(mode));
  return modes;
}

static void describe(cJSON *entities) {
  /* ---- ce qui joue ---- */
  cJSON *np = add_entity(entities, "now_playing", "Lecture en cours", "media");
  cJSON *np_bin = add_trait(np, "binary");
  cJSON_AddStringToObject(np_bin, "kind", "playing");
  cJSON *np_txt = add_trait(np, "text");
  /* Un titre se lit, il ne s'écrit pas : sans ce drapeau l'entité annoncerait
   * un `set_text` que l'enceinte ne peut pas honorer. */
  cJSON_AddBoolToObject(np_txt, "read_only", true);
  cJSON_AddStringToObject(np_txt, "label", "Titre");

  cJSON *ar =
      add_trait(add_entity(entities, "artist", "Artiste", "media"), "text");
  cJSON_AddBoolToObject(ar, "read_only", true);
  cJSON_AddStringToObject(ar, "label", "Artiste");

  /* Où on en est dans le morceau.
   *
   * L'enceinte l'a toujours su — la progression arrive dans le SET_PARAMETER
   * `text/parameters` et alimente déjà /api/nowplaying — et ne l'avait jamais
   * dit à pog Home. Ça compte plus qu'il n'y paraît : beaucoup d'émetteurs
   * (le son système d'un Mac, un onglet de navigateur) envoient la progression
   * SANS aucune métadonnée de piste, et la carte n'avait alors rien à montrer
   * qu'un « Actif ». La seule chose que l'enceinte savait vraiment restait
   * invisible.
   *
   * Un texte plutôt que deux nombres : « 1:40 / 3:14 » se lit d'un coup d'œil,
   * là où une position en secondes demanderait une durée à côté pour vouloir
   * dire quelque chose, et un curseur inviterait à chercher où sauter — ce que
   * le protocole ne permet pas ici. */
  cJSON *pr = add_trait(
      add_entity(entities, "progress", "Progression", "media"), "text");
  cJSON_AddBoolToObject(pr, "read_only", true);
  cJSON_AddStringToObject(pr, "label", "Progression");

  /* ---- transport ---- */
  cJSON *tr = add_trait(add_entity(entities, "transport", "Lecture", "media"),
                        "action");
  cJSON *cmds = cJSON_AddArrayToObject(tr, "commands");
  add_command(cmds, "play_pause", "Lecture / pause");
  add_command(cmds, "next", "Suivant");
  add_command(cmds, "prev", "Précédent");
  add_command(cmds, "volume_up", "Volume +");
  add_command(cmds, "volume_down", "Volume −");

  /* ---- son ---- */
  /* Le volume est en « media » et non en « audio » : c'est le geste quotidien,
   * il doit rester avec la lecture plutôt que derrière un onglet de réglages.
   */
  add_number(entities, "volume", "Volume", "media", 0, 100, 5, "%");
  add_select(entities, "channel", "Sortie audio", "audio", CHANNELS, 4);
  add_trait(add_entity(entities, "tone", "Correction de tonalité", "audio"),
            "on_off");
  add_number(entities, "bass", "Graves", "audio", -12, 12, 1, "dB");
  add_number(entities, "mid", "Médiums", "audio", -12, 12, 1, "dB");
  add_number(entities, "treble", "Aigus", "audio", -12, 12, 1, "dB");

  /* ---- lumière ---- */
  cJSON *lamp = add_entity(entities, "led", "Lampe", "light");
  add_trait(lamp, "on_off");
  add_trait(lamp, "brightness");
  add_modes(add_trait(lamp, "color"), "hs");
  add_light_effect_action(lamp);
  add_trait(add_entity(entities, "led_music", "Mode musique RGB", "light"),
            "on_off");
  add_select(entities, "led_effect", "Effet musical", "light", ARGB_EFFECTS,
             12);

  /* ---- protection ---- */
  add_number(entities, "amp_standby", "Veille de l'ampli", "power", 0, 120, 5,
             "min");

  /* Diagnostic, et rien de plus : c'est utile le jour où l'enceinte décroche,
   * pas un réglage qu'on vient consulter. */
  cJSON *w = add_trait(
      add_entity(entities, "wifi", "Signal WiFi", "diagnostic"), "measurement");
  cJSON_AddStringToObject(w, "kind", "rssi");
  cJSON_AddStringToObject(w, "unit", "dBm");
}

/* ================================================================== */
/*  État                                                               */
/* ================================================================== */

static cJSON *state_of(cJSON *root, const char *key) {
  return cJSON_AddObjectToObject(root, key);
}

/* La luminosité ARGB est stockée en 0..255 ; l'exposer telle quelle
 * demanderait à l'humain de raisonner en octets. */
static int pct_from_255(int v) {
  return (v * 100 + 127) / 255;
}
static int pct_to_255(double pct) {
  int v = (int)((pct * 255.0) / 100.0 + 0.5);
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void rgb_to_hs(uint32_t color, double *hue, double *saturation) {
  float r = (float)((color >> 16) & 0xFF) / 255.0f;
  float g = (float)((color >> 8) & 0xFF) / 255.0f;
  float b = (float)(color & 0xFF) / 255.0f;
  float max = fmaxf(r, fmaxf(g, b));
  float min = fminf(r, fminf(g, b));
  float delta = max - min;
  float h = 0.0f;
  if (delta > 0.0f) {
    if (max == r) {
      h = 60.0f * fmodf((g - b) / delta, 6.0f);
    } else if (max == g) {
      h = 60.0f * ((b - r) / delta + 2.0f);
    } else {
      h = 60.0f * ((r - g) / delta + 4.0f);
    }
  }
  if (h < 0.0f) {
    h += 360.0f;
  }
  *hue = h;
  *saturation = max > 0.0f ? (double)(delta / max * 100.0f) : 0.0;
}

static uint32_t hs_to_rgb(double hue, double saturation) {
  float h = fmodf((float)hue, 360.0f);
  if (h < 0.0f) {
    h += 360.0f;
  }
  float s = (float)saturation / 100.0f;
  if (s < 0.0f) {
    s = 0.0f;
  } else if (s > 1.0f) {
    s = 1.0f;
  }
  float c = s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if (h < 60.0f) {
    r = c;
    g = x;
  } else if (h < 120.0f) {
    r = x;
    g = c;
  } else if (h < 180.0f) {
    g = c;
    b = x;
  } else if (h < 240.0f) {
    g = x;
    b = c;
  } else if (h < 300.0f) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }
  float m = 1.0f - c;
  uint32_t ri = (uint32_t)((r + m) * 255.0f + 0.5f);
  uint32_t gi = (uint32_t)((g + m) * 255.0f + 0.5f);
  uint32_t bi = (uint32_t)((b + m) * 255.0f + 0.5f);
  return (ri << 16) | (gi << 8) | bi;
}

static void report(cJSON *root) {
  web_server_nowplaying_t np;
  web_server_get_nowplaying(&np);

  cJSON *o = state_of(root, "now_playing");
  cJSON_AddBoolToObject(o, "active", np.playing);
  cJSON_AddStringToObject(o, "kind", "playing");
  cJSON_AddStringToObject(o, "value", np.title);

  o = state_of(root, "artist");
  cJSON_AddStringToObject(o, "value", np.artist);

  /* Vide plutôt que « 0:00 / 0:00 » quand rien ne joue : un compteur à zéro se
   * lit comme un morceau arrêté au début, pas comme l'absence de morceau. */
  /* Borné à un peu moins de 24 h. Ce n'est pas de la place gagnée : la durée
   * arrive d'un SET_PARAMETER, donc d'un émetteur, et un paquet malformé
   * afficherait « 71582788:07 » sur une carte. Une valeur absurde vaut mieux
   * coupée que crue. */
  char progress[24] = {0};
  const uint32_t max_secs = 86399;
  if (np.playing && np.duration_secs > 0) {
    /* Publié au tick périodique et non à chaque progression : l'observateur ne
     * se déclenche pas dessus, délibérément (voir web_server.h) — une
     * publication par seconde sur le bus pour un compteur qu'on regarde de loin
     * serait le genre de trafic que le throttle du cœur existe pour refuser. */
    uint32_t pos = np.position_secs > max_secs ? max_secs : np.position_secs;
    uint32_t dur = np.duration_secs > max_secs ? max_secs : np.duration_secs;
    snprintf(progress, sizeof(progress), "%u:%02u / %u:%02u",
             (unsigned)(pos / 60), (unsigned)(pos % 60), (unsigned)(dur / 60),
             (unsigned)(dur % 60));
  }
  cJSON_AddStringToObject(state_of(root, "progress"), "value", progress);

  /* `transport` est en écriture seule — le trait `action` n'a pas d'état. On le
   * publie vide pour que le serveur voie l'entité vivante plutôt qu'absente. */
  state_of(root, "transport");

  int gain = 100;
  settings_get_max_gain(&gain);
  cJSON_AddNumberToObject(state_of(root, "volume"), "value", gain);

  int mode = settings_get_channel_mode();
  cJSON_AddStringToObject(state_of(root, "channel"), "current",
                          (mode >= 0 && mode < 4) ? CHANNELS[mode].value
                                                  : "stereo");

  bool tone_en = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&tone_en, &bass, &mid, &treble, &hpf);
  cJSON_AddBoolToObject(state_of(root, "tone"), "on", tone_en);
  cJSON_AddNumberToObject(state_of(root, "bass"), "value", bass);
  cJSON_AddNumberToObject(state_of(root, "mid"), "value", mid);
  cJSON_AddNumberToObject(state_of(root, "treble"), "value", treble);

  bool led_en = false;
  bool led_music = false;
  int gpio = -1, count = 1, fx = 0, br = 0, speed = 5;
  uint32_t color = 0;
  settings_get_argb(&led_en, &gpio, &count, &fx, &br, &color, &speed,
                    &led_music);
  o = state_of(root, "led");
  cJSON_AddBoolToObject(o, "on", led_en);
  cJSON_AddNumberToObject(o, "brightness", pct_from_255(br));
  cJSON_AddStringToObject(o, "mode", "hs");
  double hue = 0.0, saturation = 0.0;
  rgb_to_hs(color, &hue, &saturation);
  cJSON_AddNumberToObject(o, "hue", hue);
  cJSON_AddNumberToObject(o, "saturation", saturation);
  cJSON_AddBoolToObject(state_of(root, "led_music"), "on", led_music);
  {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", fx);
    cJSON_AddStringToObject(state_of(root, "led_effect"), "current", buf);
  }

  bool lim_en = true, amp_high = true;
  int lim_ceil = -1, amp_gpio = -1, standby = 5;
  settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high, &standby);
  cJSON_AddNumberToObject(state_of(root, "amp_standby"), "value", standby);

  wifi_ap_record_t ap;
  cJSON *wifi = state_of(root, "wifi");
  cJSON_AddNumberToObject(
      wifi, "value", esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);
  cJSON_AddStringToObject(wifi, "kind", "rssi");
}

/* ================================================================== */
/*  Commandes                                                          */
/* ================================================================== */

static bool num_param(const cJSON *params, const char *field, double *out) {
  const cJSON *v = cJSON_GetObjectItem(params, field);
  if (!cJSON_IsNumber(v)) {
    return false;
  }
  *out = v->valuedouble;
  return true;
}

static const char *str_param(const cJSON *params, const char *field) {
  const cJSON *v = cJSON_GetObjectItem(params, field);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Un réglage de tonalité se lit-modifie-écrit : les cinq valeurs partagent un
 * seul setter, écrire un champ sans relire les autres les remettrait à zéro. */
static void set_tone_field(const char *which, int value) {
  bool en = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&en, &bass, &mid, &treble, &hpf);
  if (strcmp(which, "bass") == 0) {
    bass = value;
  } else if (strcmp(which, "mid") == 0) {
    mid = value;
  } else if (strcmp(which, "treble") == 0) {
    treble = value;
  } else if (strcmp(which, "enabled") == 0) {
    en = value != 0;
  }
  if (settings_set_tone(en, bass, mid, treble, hpf) == ESP_OK) {
    eq_dsp_set(en, bass, mid, treble, hpf); // appliquer sans redémarrer
  }
}

static void set_argb_field(const char *which, int value) {
  bool en = false;
  bool music = false;
  int gpio = -1, count = 1, fx = 0, br = 0, speed = 5;
  uint32_t color = 0;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed, &music);
  if (strcmp(which, "enabled") == 0) {
    en = value != 0;
  } else if (strcmp(which, "effect") == 0) {
    fx = value;
  } else if (strcmp(which, "brightness") == 0) {
    br = value;
  } else if (strcmp(which, "music") == 0) {
    music = value != 0;
  }
  if (settings_set_argb(en, gpio, count, fx, br, color, speed, music) ==
      ESP_OK) {
    led_argb_reconfigure();
  }
}

static void set_argb_color(double hue, double saturation) {
  bool en = false, music = false;
  int gpio = -1, count = 1, fx = 0, br = 0, speed = 5;
  uint32_t color = 0;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed, &music);
  color = hs_to_rgb(hue, saturation);
  if (settings_set_argb(en, gpio, count, fx, br, color, speed, music) ==
      ESP_OK) {
    led_argb_reconfigure();
  }
}

static int sync_effect_index(const char *name) {
  static const struct { const char *name; int index; } effects[] = {
      {"Uni", 7},          {"Arc-en-ciel", 3},       {"Respiration", 8},
      {"Comète", 9},       {"Scintillement", 10},    {"Veilleuse", 4},
      {"VU-mètre", 0},     {"Spectre", 1},           {"Pulsation basses", 2},
      {"VU centre", 5},    {"Strobe basses", 6},     {"Niveau couleur", 11},
  };
  for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
    if (name != NULL && strcmp(name, effects[i].name) == 0) return effects[i].index;
  }
  return -1;
}

static bool set_argb_effect(const cJSON *params) {
  const char *effect_name = str_param(params, "effect");
  int effect = sync_effect_index(effect_name);
  double speed = 50, brightness = 80, hue = 0, saturation = 100;
  if (effect < 0 || !num_param(params, "speed", &speed) ||
      !num_param(params, "brightness", &brightness) ||
      !num_param(params, "primary_hue", &hue) ||
      !num_param(params, "primary_saturation", &saturation)) {
    return false;
  }
  bool enabled = false, music = false;
  int gpio = -1, count = 1, current_effect = 0, current_brightness = 0,
      current_speed = 5;
  uint32_t color = 0;
  settings_get_argb(&enabled, &gpio, &count, &current_effect,
                    &current_brightness, &color, &current_speed, &music);
  (void)enabled;
  (void)current_effect;
  (void)current_brightness;
  (void)current_speed;
  color = hs_to_rgb(hue, saturation);
  if (speed < 0) speed = 0;
  if (speed > 100) speed = 100;
  if (brightness < 0) brightness = 0;
  if (brightness > 100) brightness = 100;
  int speed10 = (int)lround(speed * 9.0 / 100.0) + 1;
  int brightness255 = pct_to_255(brightness);
  if (settings_set_argb(true, gpio, count, effect, brightness255, color,
                        speed10, true) != ESP_OK) {
    return false;
  }
  led_argb_reconfigure();
  return true;
}

static void on_command(const char *key, const char *name, const cJSON *params) {
  double n = 0;

  if (strcmp(key, "transport") == 0) {
    if (strcmp(name, "play_pause") == 0) {
      playback_control_play_pause();
    } else if (strcmp(name, "next") == 0) {
      playback_control_next();
    } else if (strcmp(name, "prev") == 0) {
      playback_control_prev();
    } else if (strcmp(name, "volume_up") == 0) {
      playback_control_volume_up();
    } else if (strcmp(name, "volume_down") == 0) {
      playback_control_volume_down();
    }
    return;
  }

  if (strcmp(key, "volume") == 0 && num_param(params, "value", &n)) {
    settings_set_max_gain((int)n);
    return;
  }

  if (strcmp(key, "channel") == 0) {
    const char *opt = str_param(params, "option");
    for (int i = 0; opt != NULL && i < 4; i++) {
      if (strcmp(opt, CHANNELS[i].value) == 0) {
        settings_set_channel_mode(i);
        return;
      }
    }
    ESP_LOGW(TAG, "mode de canal inconnu : %s", opt ? opt : "(vide)");
    return;
  }

  if (strcmp(key, "tone") == 0) {
    set_tone_field("enabled", strcmp(name, "turn_on") == 0);
    return;
  }
  if ((strcmp(key, "bass") == 0 || strcmp(key, "mid") == 0 ||
       strcmp(key, "treble") == 0) &&
      num_param(params, "value", &n)) {
    set_tone_field(key, (int)n);
    return;
  }

  if (strcmp(key, "led") == 0) {
    if (strcmp(name, "join_effect_group") == 0) {
      const char *group = str_param(params, "group_id");
      const char *role = str_param(params, "role");
      const char *leader = str_param(params, "leader_entity_id");
      const char *visualizer = str_param(params, "visualizer");
      effect_visualizer_t parsed_visualizer;
      double delay = 0, calibration = 0;
      if (!argb_follow_capable() || group == NULL || role == NULL ||
          leader == NULL || strcmp(role, "follower") != 0 ||
          !effect_sync_uuid_is_canonical(group) ||
          !effect_sync_uuid_is_canonical(leader) ||
          !effect_sync_visualizer_from_name(visualizer, &parsed_visualizer) ||
          !num_param(params, "presentation_delay_ms", &delay) ||
          !num_param(params, "calibration_offset_ms", &calibration) ||
          delay < 0 || delay > 500 || calibration < -100 ||
          calibration > 100) {
        ESP_LOGW(TAG, "join_effect_group invalide");
      } else {
        bool enabled = false;
        settings_get_argb(&enabled, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        if (!enabled) set_argb_field("enabled", true);
        if (pogdev_bus_effect_sync_join(group) == ESP_OK) {
          led_argb_effect_sync_join(group, leader, (int)delay,
                                    (int)calibration,
                                    visualizer ? visualizer : "spectrum");
        }
      }
    } else if (strcmp(name, "leave_effect_group") == 0) {
      const char *group = str_param(params, "group_id");
      if (group != NULL && pogdev_bus_effect_sync_leave(group))
        led_argb_effect_sync_leave(group);
    } else if (strcmp(name, "sync_effect") == 0) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      if (!set_argb_effect(params)) {
        ESP_LOGW(TAG, "sync_effect invalide");
      }
    } else if (strcmp(name, "turn_on") == 0) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      set_argb_field("enabled", true);
    } else if (strcmp(name, "turn_off") == 0) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      set_argb_field("enabled", false);
    } else if (strcmp(name, "toggle") == 0) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      bool enabled = false;
      settings_get_argb(&enabled, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
      set_argb_field("enabled", !enabled);
    } else if (strcmp(name, "set_brightness") == 0 &&
               num_param(params, "brightness", &n)) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      set_argb_field("brightness", pct_to_255(n));
    } else if (strcmp(name, "set_hs") == 0) {
      pogdev_bus_effect_sync_cancel();
      led_argb_effect_sync_cancel();
      double hue = 0.0, saturation = 0.0;
      if (num_param(params, "hue", &hue) &&
          num_param(params, "saturation", &saturation)) {
        set_argb_color(hue, saturation);
      }
    }
    return;
  }
  if (strcmp(key, "led_music") == 0) {
    pogdev_bus_effect_sync_cancel();
    led_argb_effect_sync_cancel();
    if (strcmp(name, "toggle") == 0) {
      bool music = false;
      settings_get_argb(NULL, NULL, NULL, NULL, NULL, NULL, NULL, &music);
      set_argb_field("music", !music);
    } else {
      set_argb_field("music", strcmp(name, "turn_on") == 0);
    }
    return;
  }
  if (strcmp(key, "led_effect") == 0) {
    pogdev_bus_effect_sync_cancel();
    led_argb_effect_sync_cancel();
    const char *opt = str_param(params, "option");
    if (opt != NULL) {
      set_argb_field("effect", atoi(opt));
    }
    return;
  }
  if (strcmp(key, "amp_standby") == 0 && num_param(params, "value", &n)) {
    bool lim_en = true, amp_high = true;
    int lim_ceil = -1, amp_gpio = -1, standby = 5;
    settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high, &standby);
    if (settings_set_protection(lim_en, lim_ceil, amp_gpio, amp_high, (int)n) ==
        ESP_OK) {
      amp_ctrl_reconfigure();
    }
    return;
  }

  ESP_LOGW(TAG, "commande sans effet : %s.%s", key, name);
}

static void on_effect_frame(const char *json, size_t length) {
  cJSON *root = cJSON_ParseWithLength(json, length);
  if (root == NULL) return;
  const cJSON *seq = cJSON_GetObjectItem(root, "seq");
  const cJSON *mono = cJSON_GetObjectItem(root, "mono_ms");
  const cJSON *lead = cJSON_GetObjectItem(root, "lead_ms");
  const cJSON *level = cJSON_GetObjectItem(root, "level");
  const cJSON *bass = cJSON_GetObjectItem(root, "bass");
  const cJSON *treble = cJSON_GetObjectItem(root, "treble");
  const cJSON *present = cJSON_GetObjectItem(root, "present_at_ms");
  if (cJSON_IsNumber(seq) && cJSON_IsNumber(mono) && cJSON_IsNumber(lead) &&
      lead->valuedouble >= 0 && lead->valuedouble <= 500 &&
      cJSON_IsNumber(level) && cJSON_IsNumber(bass) && cJSON_IsNumber(treble)) {
    led_argb_effect_sync_frame(
        (uint32_t)seq->valuedouble, (uint64_t)mono->valuedouble,
        cJSON_IsNumber(present) ? (uint64_t)present->valuedouble : 0,
        (uint16_t)lead->valuedouble, (float)level->valuedouble,
        (float)bass->valuedouble, (float)treble->valuedouble,
        (uint32_t)(esp_timer_get_time() / 1000));
  }
  cJSON_Delete(root);
}

esp_err_t pogdev_app_start(void) {
  /* Republier dès que le morceau change : sans cela pog Home afficherait le
   * titre précédent jusqu'au prochain tick périodique, soit une demi-minute de
   * retard sur un tableau de bord qu'on regarde en direct. */
  web_server_set_nowplaying_observer(pogdev_bus_notify);
  s_argb_effect_capable = argb_follow_capable();
  if (s_argb_effect_capable) pogdev_bus_enable_effect_sync(on_effect_frame);
  return pogdev_bus_start(describe, report, on_command);
}
