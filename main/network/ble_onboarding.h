/* Onboarding BLE — le transport NimBLE du protocole d'accessoires POG.
 *
 * N'existe que sur les cibles où NimBLE est activé (ESP32-S3 ; l'ESP32
 * classique porte l'A2DP Bluedroid et n'a pas la RAM pour les deux). À ne
 * démarrer que tant que `onboarding_active()` est vrai : une fois le Wi-Fi
 * configuré l'appareil redémarre et le service ne se rallume plus. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Démarre la pile NimBLE, publie le service de provisioning et annonce
 * l'appareil sous le nom « POG AirPlay ». Une seule fois par démarrage. */
esp_err_t ble_onboarding_start(void);

#ifdef __cplusplus
}
#endif
