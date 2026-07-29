#include "pogdev_app.h"

#include "amp_ctrl.h"
#include "eq_dsp.h"
#include "led_argb.h"
#include "playback_control.h"
#include "pogdev_bus.h"
#include "settings.h"
#include "web_server.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pogdev_app";

/* ================================================================== */
/*  Construction du descripteur                                        */
/* ================================================================== */

/* La catégorie est la section dans laquelle pog Home range l'entité sur la carte
 * de l'appareil. Elle ne se devine pas : `led` et `tone` portent toutes deux
 * `on_off`, `bass` et `led_brightness` toutes deux `number` — un regroupement
 * dérivé des traits mettrait les graves et la luminosité de la LED côte à côte.
 * Seul le firmware sait que `bass` appartient à l'égaliseur.
 *
 * Le vocabulaire est ouvert : une catégorie n'est jamais comparée qu'aux autres
 * catégories du MÊME appareil, donc deux firmwares qui n'emploient pas les mêmes
 * mots ne cassent rien. Ceux d'ici sont ceux du protocole (pogdev.md §4.3). */
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
                       const char *category, double min, double max, double step,
                       const char *unit) {
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

static void add_command(cJSON *cfg_commands, const char *name, const char *label) {
  cJSON *c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "name", name);
  cJSON_AddStringToObject(c, "label", label);
  /* Toutes réversibles : appuyer sur « Suivant » se rattrape avec
   * « Précédent ». Sans ce drapeau, le serveur les classerait sensibles et
   * exigerait le niveau le plus élevé pour changer de morceau. */
  cJSON_AddBoolToObject(c, "reversible", true);
  cJSON_AddItemToArray(cfg_commands, c);
}

static const option_t CHANNELS[] = {
    {"stereo", "Stéréo"}, {"mono", "Mono"}, {"left", "Gauche"}, {"right", "Droite"}};

/* L'ordre est celui des effets dans led_argb.c : l'index EST la valeur stockée
 * en NVS, il ne peut pas être réordonné sans changer ce qui est persisté. */
static const option_t ARGB_EFFECTS[] = {
    {"0", "VU-mètre"},      {"1", "Spectre"},       {"2", "Pulsation basses"},
    {"3", "Arc-en-ciel"},   {"4", "Veilleuse"},     {"5", "VU centre"},
    {"6", "Strobe basses"}, {"7", "Couleur fixe"},  {"8", "Respiration"},
    {"9", "Comète"},        {"10", "Scintillement"},{"11", "Niveau couleur"}};

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

  cJSON *ar = add_trait(add_entity(entities, "artist", "Artiste", "media"), "text");
  cJSON_AddBoolToObject(ar, "read_only", true);
  cJSON_AddStringToObject(ar, "label", "Artiste");

  /* ---- transport ---- */
  cJSON *tr = add_trait(add_entity(entities, "transport", "Lecture", "media"), "action");
  cJSON *cmds = cJSON_AddArrayToObject(tr, "commands");
  add_command(cmds, "play_pause", "Lecture / pause");
  add_command(cmds, "next", "Suivant");
  add_command(cmds, "prev", "Précédent");
  add_command(cmds, "volume_up", "Volume +");
  add_command(cmds, "volume_down", "Volume −");

  /* ---- son ---- */
  /* Le volume est en « media » et non en « audio » : c'est le geste quotidien,
   * il doit rester avec la lecture plutôt que derrière un onglet de réglages. */
  add_number(entities, "volume", "Volume", "media", 0, 100, 5, "%");
  add_select(entities, "channel", "Sortie audio", "audio", CHANNELS, 4);
  add_trait(add_entity(entities, "tone", "Correction de tonalité", "audio"), "on_off");
  add_number(entities, "bass", "Graves", "audio", -12, 12, 1, "dB");
  add_number(entities, "mid", "Médiums", "audio", -12, 12, 1, "dB");
  add_number(entities, "treble", "Aigus", "audio", -12, 12, 1, "dB");

  /* ---- lumière ---- */
  add_trait(add_entity(entities, "led", "Bande LED", "light"), "on_off");
  add_select(entities, "led_effect", "Effet lumineux", "light", ARGB_EFFECTS, 12);
  add_number(entities, "led_brightness", "Luminosité LED", "light", 0, 100, 5, "%");

  /* ---- protection ---- */
  add_number(entities, "amp_standby", "Veille de l'ampli", "power", 0, 120, 5, "min");

  /* Diagnostic, et rien de plus : c'est utile le jour où l'enceinte décroche,
   * pas un réglage qu'on vient consulter. */
  cJSON *w = add_trait(add_entity(entities, "wifi", "Signal WiFi", "diagnostic"), "measurement");
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
static int pct_from_255(int v) { return (v * 100 + 127) / 255; }
static int pct_to_255(double pct) {
  int v = (int)((pct * 255.0) / 100.0 + 0.5);
  return v < 0 ? 0 : (v > 255 ? 255 : v);
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

  /* `transport` est en écriture seule — le trait `action` n'a pas d'état. On le
   * publie vide pour que le serveur voie l'entité vivante plutôt qu'absente. */
  state_of(root, "transport");

  int gain = 100;
  settings_get_max_gain(&gain);
  cJSON_AddNumberToObject(state_of(root, "volume"), "value", gain);

  int mode = settings_get_channel_mode();
  cJSON_AddStringToObject(state_of(root, "channel"), "current",
                          (mode >= 0 && mode < 4) ? CHANNELS[mode].value : "stereo");

  bool tone_en = false;
  int bass = 0, mid = 0, treble = 0, hpf = 0;
  settings_get_tone(&tone_en, &bass, &mid, &treble, &hpf);
  cJSON_AddBoolToObject(state_of(root, "tone"), "on", tone_en);
  cJSON_AddNumberToObject(state_of(root, "bass"), "value", bass);
  cJSON_AddNumberToObject(state_of(root, "mid"), "value", mid);
  cJSON_AddNumberToObject(state_of(root, "treble"), "value", treble);

  bool led_en = false;
  int gpio = -1, count = 1, fx = 0, br = 0, speed = 5;
  uint32_t color = 0;
  settings_get_argb(&led_en, &gpio, &count, &fx, &br, &color, &speed);
  cJSON_AddBoolToObject(state_of(root, "led"), "on", led_en);
  {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", fx);
    cJSON_AddStringToObject(state_of(root, "led_effect"), "current", buf);
  }
  cJSON_AddNumberToObject(state_of(root, "led_brightness"), "value", pct_from_255(br));

  bool lim_en = true, amp_high = true;
  int lim_ceil = -1, amp_gpio = -1, standby = 5;
  settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high, &standby);
  cJSON_AddNumberToObject(state_of(root, "amp_standby"), "value", standby);

  wifi_ap_record_t ap;
  cJSON *wifi = state_of(root, "wifi");
  cJSON_AddNumberToObject(wifi, "value",
                          esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);
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
  int gpio = -1, count = 1, fx = 0, br = 0, speed = 5;
  uint32_t color = 0;
  settings_get_argb(&en, &gpio, &count, &fx, &br, &color, &speed);
  if (strcmp(which, "enabled") == 0) {
    en = value != 0;
  } else if (strcmp(which, "effect") == 0) {
    fx = value;
  } else if (strcmp(which, "brightness") == 0) {
    br = value;
  }
  if (settings_set_argb(en, gpio, count, fx, br, color, speed) == ESP_OK) {
    led_argb_reconfigure();
  }
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
    set_argb_field("enabled", strcmp(name, "turn_on") == 0);
    return;
  }
  if (strcmp(key, "led_effect") == 0) {
    const char *opt = str_param(params, "option");
    if (opt != NULL) {
      set_argb_field("effect", atoi(opt));
    }
    return;
  }
  if (strcmp(key, "led_brightness") == 0 && num_param(params, "value", &n)) {
    set_argb_field("brightness", pct_to_255(n));
    return;
  }

  if (strcmp(key, "amp_standby") == 0 && num_param(params, "value", &n)) {
    bool lim_en = true, amp_high = true;
    int lim_ceil = -1, amp_gpio = -1, standby = 5;
    settings_get_protection(&lim_en, &lim_ceil, &amp_gpio, &amp_high, &standby);
    if (settings_set_protection(lim_en, lim_ceil, amp_gpio, amp_high, (int)n) == ESP_OK) {
      amp_ctrl_reconfigure();
    }
    return;
  }

  ESP_LOGW(TAG, "commande sans effet : %s.%s", key, name);
}

esp_err_t pogdev_app_start(void) {
  /* Republier dès que le morceau change : sans cela pog Home afficherait le
   * titre précédent jusqu'au prochain tick périodique, soit une demi-minute de
   * retard sur un tableau de bord qu'on regarde en direct. */
  web_server_set_nowplaying_observer(pogdev_bus_notify);
  return pogdev_bus_start(describe, report, on_command);
}
