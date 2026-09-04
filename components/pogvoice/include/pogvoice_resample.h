#pragma once
#include <stddef.h>
#include <stdint.h>
#define POGVOICE_FIR_TAPS 32
#define POGVOICE_FIR_PHASES 32
typedef struct {
  unsigned input_rate, output_rate, phase, head;
  int16_t history[POGVOICE_FIR_TAPS];
  int16_t coefficients[POGVOICE_FIR_PHASES + 1][POGVOICE_FIR_TAPS];
} pogvoice_resampler_t;
int pogvoice_resampler_init(pogvoice_resampler_t *r, unsigned in, unsigned out);
int pogvoice_resampler_process(pogvoice_resampler_t *r, const int16_t *in,
                               size_t count, int16_t *out, size_t cap);
void pogvoice_resampler_free(pogvoice_resampler_t *r);
