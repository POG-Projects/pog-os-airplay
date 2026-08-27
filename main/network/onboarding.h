/* Onboarding BLE — la partie qui ne parle pas Bluetooth.
 *
 * Le protocole est celui des accessoires POG (service GATT 7A110000-…,
 * pog Console côté iOS via AccessorySetupKit) : l'appareil publie un lien
 * `pogconsole://provision` portant son identité et un défi ; l'app fait signer
 * une assertion par pog Auth et renvoie {challenge, ssid, password, assertion}
 * en BLE ; l'appareil enregistre le tout, redémarre, rejoint le réseau et se
 * fait adopter par pog Home sans file d'attente (voir pogdev_enrol).
 *
 * Séparé du transport pour rester lisible sans NimBLE sous les yeux — et parce
 * que valider un défi n'a rien de Bluetooth. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* L'onboarding n'a de sens que tant qu'aucun réseau n'est configuré : après,
 * l'appareil est joignable en HTTP et le BLE ne se rallume plus. */
bool onboarding_active(void);

/* Défi de session, 32 octets aléatoires en base64url. Tiré une fois par
 * démarrage : pog Auth le signe dans l'assertion et pog Home vérifie que
 * l'annonce porte le même — c'est ce qui lie l'assertion à CET appareil et à
 * CETTE session d'onboarding, pas à une capture rejouée. */
const char *onboarding_challenge(void);

/* Le lien lu par pog Console sur la caractéristique `info`. Toujours
 * reconstruit : il porte le défi de la session en cours. */
void onboarding_deep_link(char *out, size_t cap);

/* Applique une configuration reçue en BLE. Dépose la preuve pog Auth puis les
 * identifiants Wi-Fi ; ne redémarre pas (le transport choisit quand, pour
 * laisser partir la notification de statut). En cas de refus, `error` reçoit
 * un message montrable à l'humain. */
bool onboarding_apply(const char *challenge, const char *ssid,
                      const char *password, const char *assertion, char *error,
                      size_t error_cap);

#ifdef __cplusplus
}
#endif
