/* pogdev — phase 3 : le bus MQTT.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §4.
 *
 * Le composant transporte, l'application décide QUOI exposer. C'est ce qui le
 * garde utilisable tel quel sur les autres firmwares POG — une enceinte, un
 * terminal CYD et un testeur de bandes LED n'ont rien en commun sauf ce
 * protocole — et ce qui évite une dépendance vers `main`, laquelle formerait un
 * cycle que CMake écarte en silence.
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* L'application ajoute ses entités au tableau `entities` du descripteur. */
typedef void (*pogdev_describe_fn)(cJSON *entities);

/* L'application remplit `state` : une clé d'entité → un objet de champs. */
typedef void (*pogdev_state_fn)(cJSON *state);

/* Une commande reçue. `params` peut être NULL. */
typedef void (*pogdev_cmd_handler)(const char *key, const char *name, const cJSON *params);

/* Se connecte au broker avec les identifiants reçus à l'adoption, publie le
 * descripteur et l'état, puis écoute les commandes.
 *
 * Renvoie ESP_ERR_INVALID_STATE tant que l'appareil n'a pas été adopté — ce
 * n'est pas une erreur, c'est l'ordre normal des choses.
 */
esp_err_t pogdev_bus_start(pogdev_describe_fn describe, pogdev_state_fn state,
                           pogdev_cmd_handler handler);

/* Republie l'état maintenant. À appeler quand un réglage change par un autre
 * chemin — l'interface web, un bouton physique, AirPlay — sinon pog Home
 * n'apprendrait le changement qu'au prochain tick périodique. */
void pogdev_bus_notify(void);

#ifdef __cplusplus
}
#endif
