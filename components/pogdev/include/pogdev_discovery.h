/* pogdev — phase 1 : trouver le serveur pog Home sur le réseau local.
 *
 * Protocole complet : pog-docs/protocoles/pogdev.md
 *
 * Ce composant est volontairement autonome et ne touche à rien de l'amont :
 * le dépôt est un fork, et du bruit répandu dans son arbre est ce qui rend les
 * fusions pénibles. Le branchement dans main.c tient en trois lignes.
 */
#pragma once

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ce qu'un enregistrement _poghome._tcp nous apprend. */
typedef struct {
  char host[64];        /* nom résolu, p.ex. "MacBook-de-Timothy.local." */
  esp_ip4_addr_t addr;  /* adresse IPv4 résolue */
  uint16_t api_port;    /* port HTTP — c'est lui qu'on interroge */
  uint16_t mqtt_port;   /* port du broker, lu dans le TXT */
  bool tls;             /* TXT tls=1 → l'API est en HTTPS */
  int proto;            /* version du protocole annoncée par le serveur */
} pogdev_server_t;

/* Démarre la recherche en tâche de fond.
 *
 * mDNS doit déjà être initialisé (mdns_airplay_init() le fait sur cette carte) ;
 * un second mdns_init() est toléré et ignoré.
 */
esp_err_t pogdev_discovery_start(void);

/* Dernier serveur trouvé. Renvoie false tant que rien n'a été découvert. */
bool pogdev_discovery_get(pogdev_server_t *out);

#ifdef __cplusplus
}
#endif
