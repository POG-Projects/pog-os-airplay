/* pogdev — phase 3 : le bus MQTT.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §4.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ce que l'application fait d'une commande reçue.
 *
 * Le composant ne connaît ni le transport audio ni l'ampli : il transmet, et
 * l'application décide. C'est ce qui le garde utilisable tel quel sur les
 * autres firmwares POG, et ce qui évite une dépendance vers `main` — laquelle
 * formerait un cycle que CMake écarte en silence.
 */
typedef void (*pogdev_cmd_handler)(const char *key, const char *name, const cJSON *params);

/* Se connecte au broker avec les identifiants reçus à l'adoption, publie le
 * descripteur et l'état, puis écoute les commandes.
 *
 * Renvoie ESP_ERR_INVALID_STATE tant que l'appareil n'a pas été adopté — ce
 * n'est pas une erreur, c'est l'ordre normal des choses.
 */
esp_err_t pogdev_bus_start(pogdev_cmd_handler handler);

#ifdef __cplusplus
}
#endif
