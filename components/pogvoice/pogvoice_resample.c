#include "pogvoice_resample.h"
#include <math.h>
#include <string.h>

/* Polyphase, causal 32-tap lowpass. Q15 coefficients and integer phase
 * accumulation avoid the ESP32's software double arithmetic on every output
 * sample. Only initialization uses floating point. A neighboring phase blend
 * avoids quantized timing steps; input history persists across all blocks. */
int pogvoice_resampler_init(pogvoice_resampler_t *r, unsigned in,
                            unsigned out) {
  if (!r || in < 8000 || in > 48000 || out < 8000 || out > 48000)
    return -1;
  memset(r, 0, sizeof(*r));
  r->input_rate = in;
  r->output_rate = out;
  double cutoff = .94 * (out < in ? (double)out / in : 1.0);
  const double pi = 3.14159265358979323846;
  for (unsigned p = 0; p <= POGVOICE_FIR_PHASES; ++p) {
    double coefficients[POGVOICE_FIR_TAPS], sum = 0;
    for (unsigned k = 0; k < POGVOICE_FIR_TAPS; ++k) {
      double x =
          k - (POGVOICE_FIR_TAPS - 1) / 2.0 + (double)p / POGVOICE_FIR_PHASES;
      double a = 2 * pi * k / (POGVOICE_FIR_TAPS - 1);
      double window =
          .35875 - .48829 * cos(a) + .14128 * cos(2 * a) - .01168 * cos(3 * a);
      coefficients[k] =
          (fabs(x) < 1e-9 ? cutoff : sin(pi * cutoff * x) / (pi * x)) * window;
      sum += coefficients[k];
    }
    int total = 0;
    for (unsigned k = 0; k < POGVOICE_FIR_TAPS; ++k) {
      int c = (int)lround(coefficients[k] * 32768 / sum);
      r->coefficients[p][k] = (int16_t)c;
      total += c;
    }
    /* Exact unity DC gain after coefficient quantization. */
    r->coefficients[p][POGVOICE_FIR_TAPS / 2] += 32768 - total;
  }
  return 0;
}

int pogvoice_resampler_process(pogvoice_resampler_t *r, const int16_t *in,
                               size_t count, int16_t *out, size_t cap) {
  if (!r || !r->input_rate || !r->output_rate || !in || !out || count > 160 ||
      cap > 1024)
    return -1;
  unsigned span = (unsigned)count * r->output_rate;
  size_t required = span > r->phase
                        ? (span - r->phase + r->input_rate - 1) / r->input_rate
                        : 0;
  if (cap < required)
    return -1; /* Reject before changing stream state. */
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    r->history[r->head] = in[i];
    while (r->phase < r->output_rate) {
      unsigned scaled = r->phase * POGVOICE_FIR_PHASES;
      unsigned phase = scaled / r->output_rate;
      unsigned remainder = scaled % r->output_rate;
      unsigned blend = (remainder * 256) / r->output_rate;
      int64_t a = 0, b = 0;
      for (unsigned k = 0; k < POGVOICE_FIR_TAPS; ++k) {
        int32_t sample = r->history[(r->head - k) & (POGVOICE_FIR_TAPS - 1)];
        a += sample * r->coefficients[phase][k];
        b += sample * r->coefficients[phase + 1][k];
      }
      int64_t value = (a * (256 - blend) + b * blend + (1 << 22)) >> 23;
      out[used++] = value > 32767    ? 32767
                    : value < -32768 ? -32768
                                     : (int16_t)value;
      r->phase += r->input_rate;
    }
    r->phase -= r->output_rate;
    r->head = (r->head + 1) & (POGVOICE_FIR_TAPS - 1);
  }
  return (int)used;
}

void pogvoice_resampler_free(pogvoice_resampler_t *r) {
  if (r)
    memset(r, 0, sizeof(*r));
}
