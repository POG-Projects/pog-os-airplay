/* pogdev — la politique de survie du lien MQTT.
 *
 * Pure, sans dépendance ESP-IDF : compilée et exécutée sur l'hôte par la CI,
 * le même montage que hub_retry.c de pog-os-vox. Elle ne décide que des
 * MOMENTS ; les gestes (relève HTTP, bascule du client) restent au bus.
 *
 * Panne des 24-27 août 2026 : pog Home a déménagé (passage du broker sur le
 * LAN le 26), et chaque enceinte a continué de frapper à l'ancienne adresse
 * gelée en NVS — reprise par une autre machine, qui jette la connexion. Le
 * client esp-mqtt retentait sans fin, mais toujours à la même porte : les
 * lampes et capteurs POG redécouvrent le serveur quand la connexion échoue,
 * les enceintes ne le faisaient jamais. Et un CONNACK « identifiants
 * refusés » conseillait d'effacer la NVS à la main, alors qu'un courtier qui
 * redémarre refuse aussi pendant que ses comptes se reprovisionnent — la
 * relève auprès de pog Home tranche, jamais le CONNACK seul. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trois échecs de suite (~4 min 30 au rythme du client) avant d'aller voir si
 * le serveur a déménagé : un câble débranché deux minutes ne justifie pas de
 * toucher à la NVS. */
#define POGDEV_RETRY_ECHECS_AVANT_SUIVI 3

/* Les refus d'authentification doivent persister — plusieurs, étalés — avant
 * une relève, et les relèves restent espacées : pog Home limite les annonces
 * par adresse IP. */
#define POGDEV_RETRY_REFUS_AVANT_RELEVE   3
#define POGDEV_RETRY_FENETRE_REFUS_MS     90000u
#define POGDEV_RETRY_ESPACEMENT_RELEVE_MS 120000u

typedef struct {
  uint8_t echecs; /* connexions tombées ou refusées, consécutives */
  uint8_t refus;  /* CONNACK « identifiants refusés » consécutifs */
  bool premier_refus_pose;
  uint32_t premier_refus_ms;
  bool releve_posee;
  uint32_t derniere_releve_ms;
} pogdev_retry_t;

void pogdev_retry_init(pogdev_retry_t *r);

/* CONNACK accepté : tout repart de zéro. Le TCP établi ne suffit pas — un
 * serveur qui accepte puis rejette en boucle garderait sinon la politique
 * muette. */
void pogdev_retry_connecte(pogdev_retry_t *r);

/* Une connexion tombée ou impossible. Rend true quand l'échec est assez
 * installé pour vérifier auprès de la découverte si le serveur a déménagé —
 * puis à chaque échec suivant : le suivi est une comparaison d'adresses, il
 * ne coûte que quand quelque chose a vraiment changé. */
bool pogdev_retry_echec(pogdev_retry_t *r);

/* Un CONNACK « identifiants refusés ». Rend true quand une relève auprès de
 * pog Home est due ; l'horloge est millis-compatible, le débordement de
 * l'entier (49,7 jours) ne fausse pas les fenêtres. */
bool pogdev_retry_refus_auth(pogdev_retry_t *r, uint32_t maintenant_ms);

#ifdef __cplusplus
}
#endif
