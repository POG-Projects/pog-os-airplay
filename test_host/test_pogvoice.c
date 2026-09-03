#include "pogvoice_protocol.h"
#include "pogvoice_resample.h"
#include "pogvoice_playout.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void protocol(void) {
  pogvoice_origin_t o;
  assert(pogvoice_origin_parse("https://AI.example:443", false, &o));
  assert(o.secure && o.port == 443 && !strcmp(o.host, "ai.example"));
  assert(pogvoice_ws_origin_allowed("http://192.168.129.50:8766",
                                    "ws://192.168.129.50:8765", true));
  assert(!pogvoice_ws_origin_allowed("http://192.168.129.50:8766",
                                     "ws://192.168.129.50:8765", false));
  assert(!pogvoice_ws_origin_allowed("https://ai.example", "ws://ai.example",
                                     true));
  assert(!pogvoice_ws_origin_allowed("https://ai.example",
                                     "wss://attacker.example", true));
  assert(!pogvoice_origin_parse("https://user:pw@host", false, &o));
  assert(!pogvoice_origin_parse("http://host:65536", false, &o));
  assert(!pogvoice_origin_parse("http://host:0", false, &o));
  assert(!pogvoice_origin_parse("http://host/path", false, &o));
  assert(!pogvoice_origin_parse("http://host?key=x", false, &o));
  assert(!pogvoice_origin_parse("http://host\r\nX:evil", false, &o));
  assert(!pogvoice_origin_parse("file://host", false, &o));

  uint8_t packet[1300], type;
  const uint8_t audio[] = {0xf8, 0xff, 0xfe};
  const uint8_t *payload;
  size_t length;
  assert(pogvoice_v3_encode(packet, sizeof(packet), audio, 3) == 7);
  const uint8_t expected[] = {0, 0, 0, 3, 0xf8, 0xff, 0xfe};
  assert(!memcmp(packet, expected, 7));
  assert(pogvoice_v3_decode(packet, 7, &type, &payload, &length));
  assert(type == 0 && length == 3 && !memcmp(payload, audio, 3));
  assert(!pogvoice_v3_decode(packet, 6, &type, &payload, &length));
  packet[0] = 2;
  assert(!pogvoice_v3_decode(packet, 7, &type, &payload, &length));
  assert(!pogvoice_v3_encode(packet, 6, audio, 3));

  pogvoice_message_t m = {0};
  assert(pogvoice_message_feed(&m, 1, false, 0, 4, "he", 2) == 0);
  assert(pogvoice_message_feed(&m, 9, true, 0, 1, "x", 1) ==
         0); /* interleaved ping */
  assert(pogvoice_message_feed(&m, 1, false, 2, 4, "ll", 2) == 0);
  assert(pogvoice_message_feed(&m, 0, true, 0, 1, "o", 1) == 1);
  assert(m.opcode == 1 && m.used == 5 && !memcmp(m.bytes, "hello", 6));
  assert(pogvoice_message_feed(&m, 0, true, 0, 1, "x", 1) == -1);
  assert(pogvoice_message_feed(&m, 2, true, 0, 7, expected, 3) == 0);
  assert(pogvoice_message_feed(&m, 2, true, 3, 7, expected + 3, 4) == 1);
  assert(m.opcode == 2 && m.used == 7);
  assert(pogvoice_message_feed(&m, 1, true, 0, 5000, "x", 1) == -1);
  assert(pogvoice_message_feed(&m, 1, true, 0, 4, "x", 1) == 0);
  assert(pogvoice_message_feed(&m, 1, true, 2, 4, "x", 1) == -1);
  assert(pogvoice_message_feed(&m, 1, true, 0, 0, NULL, 0) == 1);
}

static double tone(unsigned input_rate, unsigned output_rate, double hz,
                   size_t block) {
  pogvoice_resampler_t *r = calloc(1, sizeof(*r));
  assert(r && pogvoice_resampler_init(r, input_rate, output_rate) == 0);
  size_t total = 0;
  double energy = 0;
  size_t measured = 0;
  int16_t input[160], output[1024];
  for (size_t i = 0; i < input_rate; i += block) {
    size_t n = input_rate - i;
    if (n > block)
      n = block;
    for (size_t j = 0; j < n; j++)
      input[j] =
          (int16_t)(10000 * sin(6.283185307179586 * hz * (i + j) / input_rate));
    int got = pogvoice_resampler_process(r, input, n, output, 1024);
    assert(got >= 0);
    for (int j = 0; j < got; j++)
      if (total + (size_t)j > 200) {
        double v = output[j];
        energy += v * v;
        measured++;
      }
    total += (size_t)got;
  }
  assert(total > output_rate - 100 && total < output_rate + 100);
  pogvoice_resampler_free(r);
  free(r);
  return sqrt(energy / measured);
}
static void playout(void) {
  pogvoice_playout_t p = {0};
  /* Fragmented arrivals must not become little PCM/silence bursts. */
  assert(pogvoice_playout_take(&p, 294, 353, 10584) == 0);
  assert(pogvoice_playout_take(&p, 10000, 353, 10584) == 0);
  assert(pogvoice_playout_take(&p, 10584, 353, 10584) == 353);
  assert(pogvoice_playout_take(&p, 354, 353, 10584) == 353);
  assert(pogvoice_playout_take(&p, 294, 353, 10584) == 0);
  assert(p.underruns == 1 && p.played_samples == 706);
  assert(pogvoice_playout_take(&p, 1000, 353, 10584) == 0);
  assert(p.underruns == 1);
  p.finished = true;
  assert(pogvoice_playout_take(&p, 294, 353, 10584) == 294);
  assert(pogvoice_playout_take(&p, 0, 353, 10584) == 0);
  assert(p.played_samples == 1000 && p.underruns == 1);
  /* A response shorter than the prebuffer must still play in full. */
  p = (pogvoice_playout_t){.finished = true};
  assert(pogvoice_playout_take(&p, 120, 353, 10584) == 120);
  assert(p.underruns == 0);
}

int main(void) {
  playout();
  protocol();
  double pass = tone(44100, 16000, 1000, 160),
         alias = tone(44100, 16000, 14000, 160);
  assert(pass > 6500 && pass < 7500 && alias < 500);
  assert(fabs(tone(44100, 16000, 1000, 37) - pass) < 10);
  assert(tone(24000, 44100, 1000, 160) > 6500);
  assert(tone(48000, 16000, 1000, 160) > 6500);
  assert(tone(8000, 48000, 1000, 160) > 6000);
  puts("pogvoice: framing, endpoint trust and audio rates OK");
}
