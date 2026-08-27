/* La politique de survie du lien MQTT, vérifiée sur l'hôte — le même montage
 * que test_hub_retry.c de pog-os-vox. Aucun matériel, aucun réseau. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "pogdev_retry.h"

int main(void) {
  /* -- Les échecs de transport : le suivi ne se déclenche qu'installé. -- */
  pogdev_retry_t r;
  pogdev_retry_init(&r);
  assert(!pogdev_retry_echec(&r));
  assert(!pogdev_retry_echec(&r));
  assert(pogdev_retry_echec(&r));
  /* Puis à chaque échec : le suivi n'est qu'une comparaison d'adresses, il ne
   * bascule que si le serveur a vraiment bougé. Un serveur absent des heures
   * ne fait jamais taire la demande — c'est la panne d'août qu'on rejoue. */
  for (int i = 0; i < 10000; i++) {
    assert(pogdev_retry_echec(&r));
  }
  /* Le CONNACK accepté remet tout à zéro ; le TCP établi seul, jamais. */
  pogdev_retry_connecte(&r);
  assert(!pogdev_retry_echec(&r));

  /* -- Les refus d'identifiants : demander avant d'oublier. -- */
  pogdev_retry_t g;
  pogdev_retry_init(&g);
  assert(!pogdev_retry_refus_auth(&g, 1000));
  assert(!pogdev_retry_refus_auth(&g, 6000));
  /* Trois refus mais trop rapprochés : un broker qui redémarre passe. */
  assert(!pogdev_retry_refus_auth(&g, 11000));
  /* Les refus persistent au-delà de la fenêtre : la relève devient due. */
  assert(pogdev_retry_refus_auth(&g, 91000));
  /* ...et reste polie ensuite (pog Home limite les annonces par IP). */
  assert(!pogdev_retry_refus_auth(&g, 151000));
  assert(pogdev_retry_refus_auth(&g, 211000));
  /* Un CONNACK accepté referme tout. */
  pogdev_retry_connecte(&g);
  assert(!pogdev_retry_refus_auth(&g, 500000));
  assert(!pogdev_retry_refus_auth(&g, 505000));

  /* -- Le débordement de millis (49,7 jours) ne fausse pas les fenêtres :
   * un appareil mural atteint cette échéance en vrai. -- */
  pogdev_retry_t w;
  pogdev_retry_init(&w);
  uint32_t bord = UINT32_MAX - 1000u;
  assert(!pogdev_retry_refus_auth(&w, bord));
  assert(!pogdev_retry_refus_auth(&w, bord + 5000u));
  assert(!pogdev_retry_refus_auth(&w, bord + 10000u));
  assert(pogdev_retry_refus_auth(&w, bord + 95000u));

  puts("politique de survie du lien MQTT : OK");
  return 0;
}
