#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POGVOICE_MESSAGE_MAX 4096
typedef struct {
  char scheme[6];
  char host[128];
  uint16_t port;
  bool secure;
} pogvoice_origin_t;
/* Explicit origins only: no userinfo, path, query, fragment or IPv6 literal. */
bool pogvoice_origin_parse(const char *url, bool websocket,
                           pogvoice_origin_t *out);
bool pogvoice_ws_origin_allowed(const char *api, const char *ws,
                                bool allow_lan);
bool pogvoice_v3_decode(const uint8_t *data, size_t size, uint8_t *type,
                        const uint8_t **payload, size_t *length);
size_t pogvoice_v3_encode(uint8_t *out, size_t capacity, const uint8_t *opus,
                          size_t len);

/* Reassemble IDF chunks and RFC6455 continuation frames. Control frames may
 * interleave. Invalid ordering/oversize input fails closed. */
typedef struct {
  uint8_t bytes[POGVOICE_MESSAGE_MAX + 1];
  size_t used, frame_used, frame_size;
  uint8_t opcode;
  bool open, in_frame;
} pogvoice_message_t;
int pogvoice_message_feed(pogvoice_message_t *m, uint8_t opcode, bool fin,
                          size_t offset, size_t frame_size, const void *data,
                          size_t length);
