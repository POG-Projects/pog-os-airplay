#include "pogvoice_protocol.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool pogvoice_origin_parse(const char *url, bool ws, pogvoice_origin_t *out) {
  if (!url || !out || strlen(url) > 200)
    return false;
  memset(out, 0, sizeof(*out));
  const char *sep = strstr(url, "://");
  if (!sep || sep - url > 5)
    return false;
  memcpy(out->scheme, url, (size_t)(sep - url));
  if (strcmp(out->scheme, ws ? "wss" : "https") == 0)
    out->secure = true;
  else if (strcmp(out->scheme, ws ? "ws" : "http") != 0)
    return false;
  const char *p = sep + 3, *host = p;
  while (*p && *p != ':' && *p != '/') {
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-')
      return false;
    p++;
  }
  size_t n = p - host;
  if (!n || n >= sizeof(out->host) || host[0] == '-' || host[n - 1] == '-')
    return false;
  for (size_t i = 0; i < n; i++)
    out->host[i] = (char)tolower((unsigned char)host[i]);
  out->port = out->secure ? 443 : 80;
  if (*p == ':') {
    p++;
    if (!isdigit((unsigned char)*p))
      return false;
    unsigned port = 0;
    while (isdigit((unsigned char)*p)) {
      port = port * 10 + (unsigned)(*p++ - '0');
      if (port > 65535)
        return false;
    }
    if (!port)
      return false;
    out->port = (uint16_t)port;
  }
  return *p == '\0' || (p[0] == '/' && p[1] == '\0');
}

bool pogvoice_ws_origin_allowed(const char *api, const char *ws, bool lan) {
  pogvoice_origin_t a, b;
  return pogvoice_origin_parse(api, false, &a) &&
         pogvoice_origin_parse(ws, true, &b) && !strcmp(a.host, b.host) &&
         ((a.secure && b.secure) || (lan && !a.secure && !b.secure));
}

bool pogvoice_v3_decode(const uint8_t *d, size_t n, uint8_t *type,
                        const uint8_t **payload, size_t *len) {
  if (!d || !type || !payload || !len || n < 4 || d[0] > 1 || d[1] != 0 ||
      n - 4 != ((size_t)d[2] << 8 | d[3]))
    return false;
  *type = d[0];
  *payload = d + 4;
  *len = n - 4;
  return true;
}
size_t pogvoice_v3_encode(uint8_t *out, size_t cap, const uint8_t *opus,
                          size_t n) {
  if (!out || !opus || n == 0 || n > 1275 || cap < n + 4)
    return 0;
  out[0] = out[1] = 0;
  out[2] = (uint8_t)(n >> 8);
  out[3] = (uint8_t)n;
  memcpy(out + 4, opus, n);
  return n + 4;
}

int pogvoice_message_feed(pogvoice_message_t *m, uint8_t op, bool fin,
                          size_t off, size_t total, const void *data,
                          size_t len) {
  if (op >= 8)
    return 0;
  if (!m || (len && !data) || off > total || len > total - off)
    return -1;
  if (off == 0) {
    if (m->in_frame || (op != 0 && op != 1 && op != 2))
      goto bad;
    if (op == 0) {
      if (!m->open)
        goto bad;
    } else {
      if (m->open)
        goto bad;
      m->used = 0;
      m->opcode = op;
      m->open = true;
    }
    if (total > POGVOICE_MESSAGE_MAX - m->used)
      goto bad;
    m->frame_used = 0;
    m->frame_size = total;
    m->in_frame = true;
  }
  if (!m->in_frame || off != m->frame_used || total != m->frame_size ||
      len > POGVOICE_MESSAGE_MAX - m->used)
    goto bad;
  if (len)
    memcpy(m->bytes + m->used, data, len);
  m->used += len;
  m->frame_used += len;
  if (m->frame_used != total)
    return 0;
  m->in_frame = false;
  if (!fin)
    return 0;
  m->open = false;
  m->bytes[m->used] = 0;
  return 1;
bad:
  memset(m, 0, sizeof(*m));
  return -1;
}
