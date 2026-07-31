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

/* Version de firmware annoncée à l'enrôlement et dans le `hello` ; c'est elle
 * que pog Home enregistre dans `devices.sw_version`.
 *
 * Elle est lue dans le descripteur d'application ESP-IDF, donc dans
 * PROJECT_VER, donc dans version.txt : la même source que `firmware_version` de
 * /api/system/info et que la comparaison OTA. Le choix précédent — un littéral
 * figé dans cet en-tête — laissait tout le parc annoncer 0.1.0 : deux firmwares
 * différents devenaient indiscernables, et un `hello` légitime a été pris pour
 * un rejeu.
 *
 * Ce n'est pas la version du protocole pogdev, qui voyage à part : `proto` sur
 * le bus, `proto_version` à l'annonce. */
const char *pogdev_fw_version(void);

/**
 * Erase this device's enrolment: its credentials, its broker address and its
 * claim secret.
 *
 * There was no way to do this over the network, and that turned an ordinary
 * accident into a soldering-iron problem. A firmware update wiped a speaker's
 * MQTT settings; the server had no matching account any more; and the device
 * could not go back to the enrolment queue because `enrol_task` reads NVS first
 * and deletes itself the moment it finds credentials. It sat on the LAN,
 * healthy and refused by the broker every ten seconds, and the only way out was
 * USB.
 *
 * The caller MUST restart afterwards, and that is not a courtesy: `enrol_task`
 * has already deleted itself by then, so nothing re-reads this. The erase alone
 * changes nothing until the next boot.
 *
 * Erases the whole namespace rather than key by key — a half-erased enrolment
 * is worse than either state, since `load_state()` needs three keys together
 * and would find some of them.
 */
esp_err_t pogdev_forget(void);

#ifdef __cplusplus
}
#endif
