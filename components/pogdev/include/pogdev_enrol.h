/* pogdev — phase 2 : adoption.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §3.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version de firmware annoncée. Volontairement distincte de celle de
 * l'application AirPlay : elle dit ce que l'appareil sait faire du protocole
 * pogdev, pas ce qu'il sait faire de l'audio. */
#define POGDEV_FW_VERSION "0.1.0"

/* Modèle annoncé à l'adoption et dans le descripteur. Une seule définition,
 * pour que les deux ne puissent pas diverger. */
#define POGDEV_MODEL "POG AirPlay (XIAO S3)"

/* Ce qu'on reçoit à l'adoption, une seule fois. */
typedef struct {
  char device_id[48]; /* identifiant attribué par pog Home */
  char mqtt_host[48];
  uint16_t mqtt_port;
  char mqtt_password[80]; /* 256 bits en hexadécimal */
} pogdev_creds_t;

/* Démarre l'enrôlement en tâche de fond : s'annonce tant qu'on n'est pas
 * adopté, puis relève les identifiants et les enregistre en NVS. Sans effet si
 * l'appareil est déjà enrôlé.
 *
 * `device_name` est le nom proposé à l'humain au moment d'adopter ; il est
 * passé en paramètre plutôt que lu depuis les réglages, pour que le composant
 * ne dépende de rien de l'application — un composant qui dépendrait de `main`
 * formerait un cycle, et CMake l'écarte silencieusement. */
esp_err_t pogdev_enrol_start(const char *device_name);

/* Identifiants enregistrés. false tant que l'appareil n'a pas été adopté. */
bool pogdev_enrol_get(pogdev_creds_t *out);

/* Identifiant matériel stable, celui que l'humain compare à l'écran. */
const char *pogdev_hw_id(void);

#ifdef __cplusplus
}
#endif
