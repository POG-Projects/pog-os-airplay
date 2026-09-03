/* pogdev — phase 2 : adoption.
 *
 * Protocole : pog-docs/protocoles/pogdev.md §3.
 */
#pragma once

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modèle annoncé à l'adoption et dans le descripteur. Une seule définition,
 * pour que les deux ne puissent pas diverger. */
#if defined(CONFIG_BOARD_WROVER_E) && CONFIG_BOARD_WROVER_E
#define POGDEV_MODEL "POG AirPlay (WROVER-E)"
#elif defined(POG_BOARD_XIAO_S3) || \
    (defined(CONFIG_BOARD_XIAO_ESP32S3) && CONFIG_BOARD_XIAO_ESP32S3)
#define POGDEV_MODEL "POG AirPlay (XIAO S3)"
#else
#define POGDEV_MODEL "POG AirPlay"
#endif

/* Classe d'appareil, annoncée à l'adoption ET portée par le lien d'onboarding
 * BLE. pog Auth signe l'assertion sur {hw_id, challenge, device_class, model}
 * et pog Home vérifie l'égalité stricte avec l'annonce : une divergence entre
 * les deux usages rend toute adoption automatique impossible, d'où la
 * définition unique. La valeur doit exister dans la liste fermée de
 * pog-srv-auth (device_provisioning.go) et de poghome (enrolment.go). */
#define POGDEV_DEVICE_CLASS "airplay_speaker"

/* Ce qu'on reçoit à l'adoption, une seule fois. */
typedef struct {
  char device_id[48]; /* identifiant attribué par pog Home */
  char mqtt_host[48];
  uint16_t mqtt_port;
  char mqtt_password[80]; /* 256 bits en hexadécimal */
} pogdev_creds_t;

/* Appelé une fois, juste après que les identifiants d'adoption ont été relevés
 * et écrits en NVS.
 *
 * Il existe parce que le bus MQTT ne démarrait qu'au boot : un appareil adopté
 * depuis pog Console restait enrôlé et muet jusqu'à ce que quelqu'un aille le
 * redémarrer. Il apparaissait dans la maison sans une seule capacité, ce qui se
 * lit comme une adoption ratée.
 *
 * C'est un rappel et non un appel direct : `main` démarre le bus, et un
 * composant qui dépendrait de `main` formerait le cycle que CMake écarte
 * silencieusement — la raison pour laquelle `device_name` est déjà un
 * paramètre.
 *
 * Appelé depuis la tâche d'enrôlement, une seule fois, juste avant qu'elle se
 * termine. */
typedef void (*pogdev_adopted_cb)(void);

/* Démarre l'enrôlement en tâche de fond : s'annonce tant qu'on n'est pas
 * adopté, puis relève les identifiants et les enregistre en NVS. Sans effet si
 * l'appareil est déjà enrôlé.
 *
 * `device_name` est le nom proposé à l'humain au moment d'adopter ; il est
 * passé en paramètre plutôt que lu depuis les réglages, pour que le composant
 * ne dépende de rien de l'application — un composant qui dépendrait de `main`
 * formerait un cycle, et CMake l'écarte silencieusement. */
esp_err_t pogdev_enrol_start(const char *device_name,
                             pogdev_adopted_cb on_adopted);

/* Identifiants enregistrés. false tant que l'appareil n'a pas été adopté. */
bool pogdev_enrol_get(pogdev_creds_t *out);

/* Identifiant matériel stable, celui que l'humain compare à l'écran. */
const char *pogdev_hw_id(void);

/* Dépose la preuve d'onboarding reçue en BLE : le challenge annoncé par
 * l'appareil et l'assertion signée par pog Auth. Elle est écrite en NVS parce
 * que l'appareil redémarre entre sa réception et son usage — c'est la
 * prochaine annonce, après jonction du Wi-Fi, qui la présentera à pog Home
 * pour être adopté sans passer par la file d'attente. Consommée (effacée) à
 * l'adoption, ou abandonnée si le serveur la refuse : l'assertion expire en
 * deux minutes, et une preuve périmée ne doit pas condamner l'appareil à
 * réannoncer un refus pour toujours. */
esp_err_t pogdev_provisioning_store(const char *challenge,
                                    const char *assertion);

/* Oublie la preuve sans toucher au reste de l'enrôlement. Utilisé quand la
 * configuration Wi-Fi échoue après le dépôt : une preuve orpheline serait
 * présentée avec un état incohérent. */
esp_err_t pogdev_provisioning_clear(void);

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

/* Relève les identifiants avec le secret de réclamation déjà en NVS, auprès du
 * serveur que la découverte connaît MAINTENANT.
 *
 * C'est la réponse aux CONNACK « identifiants refusés » qui persistent : le
 * serveur tranche, jamais le CONNACK seul — un courtier qui redémarre refuse
 * aussi pendant que ses comptes se reprovisionnent, alors que l'oubli est
 * irréversible sans un humain. Trois issues côté pog Home : 404 = vraiment
 * oublié, « pending » = demande de ré-adoption posée et visible dans
 * l'inventaire, « adopted » = identifiants neufs écrits en NVS.
 *
 * Rend true seulement quand des identifiants neufs ont été enregistrés ;
 * l'appelant relance alors le client MQTT. Fait du HTTP : jamais depuis un
 * handler d'événement. */
bool pogdev_enrol_recollect(void);

/* Suit un serveur qui a déménagé : réécrit l'hôte et le port MQTT en NVS quand
 * ils diffèrent de ce que la découverte vient de résoudre.
 *
 * L'adresse était gelée à la relève des identifiants et plus jamais relue :
 * quand pog Home est passé sur le LAN le 26 août 2026, chaque enceinte a
 * continué de frapper à l'ancienne adresse — reprise depuis par une autre
 * machine — jusqu'au fer à souder. Les lampes et capteurs POG suivent le
 * serveur ; ce composant doit le faire aussi, sans reflash.
 *
 * Rend true seulement quand quelque chose a changé et a été persisté. */
bool pogdev_enrol_update_host(const esp_ip4_addr_t *addr, uint16_t mqtt_port);

#ifdef __cplusplus
}
#endif
