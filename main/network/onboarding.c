#include "onboarding.h"

#include "esp_random.h"
#include "pogdev_enrol.h"
#include "settings.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bornes du protocole, partagées avec pog Console et pog Home. Le SSID et le
 * mot de passe sont les limites 802.11 ; l'assertion est un JWS compact dont
 * la forme exacte n'est vérifiée que par pog Home — ici on écarte seulement ce
 * qui ne peut manifestement pas en être un. */
#define ONBOARDING_SSID_MAX      32
#define ONBOARDING_PASSWORD_MAX  64
#define ONBOARDING_ASSERTION_MIN 128
#define ONBOARDING_ASSERTION_MAX 3072

static char s_challenge[44];

bool onboarding_active(void) {
  return !settings_has_wifi_credentials();
}

/* base64url sans bourrage, l'alphabet attendu par pog Auth. */
static void base64url(const uint8_t *data, size_t len, char *out) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  uint32_t buffer = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    buffer = (buffer << 8) | data[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out[n++] = alphabet[(buffer >> bits) & 0x3f];
    }
  }
  if (bits > 0) {
    out[n++] = alphabet[(buffer << (6 - bits)) & 0x3f];
  }
  out[n] = '\0';
}

const char *onboarding_challenge(void) {
  if (s_challenge[0] == '\0') {
    uint8_t raw[32];
    esp_fill_random(raw, sizeof(raw));
    base64url(raw, sizeof(raw), s_challenge);
  }
  return s_challenge;
}

/* Encodage pourcent, conservateur : tout ce qui n'est pas sûr partout est
 * encodé, pour que le lien survive à n'importe quel analyseur d'URL. */
static void append_url_encoded(char *out, size_t cap, size_t *used,
                               const char *value) {
  for (const char *p = value; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~';
    if (safe) {
      if (*used + 1 >= cap) {
        return;
      }
      out[(*used)++] = (char)c;
      out[*used] = '\0';
    } else {
      if (*used + 3 >= cap) {
        return;
      }
      snprintf(out + *used, cap - *used, "%%%02X", c);
      *used += 3;
    }
  }
}

void onboarding_deep_link(char *out, size_t cap) {
  /* L'endpoint est l'adresse du portail de secours (le SoftAP reste ouvert
   * pendant l'onboarding) ; pog Console exige précisément cet hôte. */
  size_t used = (size_t)snprintf(
      out, cap,
      "pogconsole://provision?endpoint=http%%3A%%2F%%2F192.168.4.1&hw_id=%s"
      "&challenge=%s&device_class=" POGDEV_DEVICE_CLASS "&model=",
      pogdev_hw_id(), onboarding_challenge());
  if (used >= cap) {
    out[cap > 0 ? cap - 1 : 0] = '\0';
    return;
  }
  append_url_encoded(out, cap, &used, POGDEV_MODEL);
}

bool onboarding_apply(const char *challenge, const char *ssid,
                      const char *password, const char *assertion, char *error,
                      size_t error_cap) {
  if (!onboarding_active()) {
    snprintf(error, error_cap, "L’onboarding est déjà terminé.");
    return false;
  }
  size_t ssid_len = ssid != NULL ? strlen(ssid) : 0;
  size_t password_len = password != NULL ? strlen(password) : 0;
  size_t assertion_len = assertion != NULL ? strlen(assertion) : 0;
  if (challenge == NULL || strcmp(challenge, onboarding_challenge()) != 0 ||
      ssid_len == 0 || ssid_len > ONBOARDING_SSID_MAX ||
      password_len > ONBOARDING_PASSWORD_MAX ||
      assertion_len < ONBOARDING_ASSERTION_MIN ||
      assertion_len > ONBOARDING_ASSERTION_MAX ||
      strchr(assertion, '.') == NULL) {
    snprintf(error, error_cap, "La preuve POG Auth ou le réseau est invalide.");
    return false;
  }
  /* La preuve d'abord : si elle ne peut pas être écrite, mieux vaut échouer
   * avant d'avoir touché au Wi-Fi et laisser l'humain réessayer. */
  if (pogdev_provisioning_store(challenge, assertion) != ESP_OK) {
    snprintf(error, error_cap, "La configuration n’a pas pu être enregistrée.");
    return false;
  }
  if (settings_set_wifi_credentials(ssid, password) != ESP_OK) {
    /* Une preuve sans réseau serait annoncée dans un état incohérent. */
    pogdev_provisioning_clear();
    snprintf(error, error_cap, "La configuration n’a pas pu être enregistrée.");
    return false;
  }
  return true;
}
