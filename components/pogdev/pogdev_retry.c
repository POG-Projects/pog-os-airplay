#include "pogdev_retry.h"

void pogdev_retry_init(pogdev_retry_t *r) {
  pogdev_retry_t vierge = {0};
  *r = vierge;
}

void pogdev_retry_connecte(pogdev_retry_t *r) {
  pogdev_retry_init(r);
}

bool pogdev_retry_echec(pogdev_retry_t *r) {
  if (r->echecs < 255) {
    r->echecs++;
  }
  return r->echecs >= POGDEV_RETRY_ECHECS_AVANT_SUIVI;
}

bool pogdev_retry_refus_auth(pogdev_retry_t *r, uint32_t maintenant_ms) {
  if (!r->premier_refus_pose) {
    r->premier_refus_pose = true;
    r->premier_refus_ms = maintenant_ms;
  }
  if (r->refus < 255) {
    r->refus++;
  }
  if (r->refus < POGDEV_RETRY_REFUS_AVANT_RELEVE) {
    return false;
  }
  /* Arithmétique non signée : correcte à travers le débordement de millis. */
  if (maintenant_ms - r->premier_refus_ms < POGDEV_RETRY_FENETRE_REFUS_MS) {
    return false;
  }
  if (r->releve_posee && maintenant_ms - r->derniere_releve_ms <
                             POGDEV_RETRY_ESPACEMENT_RELEVE_MS) {
    return false;
  }
  r->releve_posee = true;
  r->derniere_releve_ms = maintenant_ms;
  return true;
}
