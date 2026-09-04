#include "pogvoice_feedback.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  uint8_t rgb[300 * 3 + 1];
  memset(rgb, 42, sizeof(rgb));
  assert(!pogvoice_feedback_render(rgb, 300, POGVOICE_LIGHT_OFF, 0, 1));
  assert(rgb[0] == 42); /* No override means leave the normal lamp alone. */
  for (int state = POGVOICE_LIGHT_CONNECTING; state <= POGVOICE_LIGHT_ERROR; state++) {
    assert(pogvoice_feedback_render(rgb, 300, state, 400, NAN));
    assert(rgb[900] == 42);
    assert(pogvoice_feedback_render(rgb, 1, state, 0, INFINITY));
    assert(!pogvoice_feedback_render(rgb, 0, state, 0, 0));
  }
  assert(pogvoice_feedback_render(rgb, 10, POGVOICE_LIGHT_LISTENING, 0, 0));
  unsigned quiet = rgb[1];
  assert(pogvoice_feedback_render(rgb, 10, POGVOICE_LIGHT_LISTENING, 0, 1));
  assert(rgb[1] > quiet && rgb[2] > rgb[1] && rgb[0] == 0);
  assert(pogvoice_feedback_render(rgb, 10, POGVOICE_LIGHT_SPEAKING, 0, 1));
  assert(rgb[1] > rgb[0] && rgb[1] > rgb[2]);
  memset(rgb, 42, sizeof(rgb));
  assert(!pogvoice_feedback_render(rgb, 300, POGVOICE_LIGHT_ERROR, 4000, 0));
  assert(rgb[0] == 42); /* Error indication expires and releases the lamp. */
  puts("pogvoice_feedback: OK");
}
