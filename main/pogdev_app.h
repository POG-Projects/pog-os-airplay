/* Ce que cette enceinte expose à pog Home.
 *
 * Le composant `pogdev` porte le protocole ; ce fichier porte le contenu. La
 * séparation n'est pas cosmétique : elle évite une dépendance du composant vers
 * `main`, laquelle formerait un cycle que CMake écarte en silence, et garde le
 * composant réutilisable tel quel sur les autres firmwares POG.
 */
#pragma once

#include "esp_err.h"

/* Démarre le bus en lui passant le descripteur, l'état et le gestionnaire de
 * commandes de cette enceinte. Renvoie ESP_ERR_INVALID_STATE tant que
 * l'appareil n'a pas été adopté — ce n'est pas une erreur. */
esp_err_t pogdev_app_start(void);
