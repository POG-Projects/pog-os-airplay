#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pogwake_engine pogwake_engine_t;
const char *pogwake_engine_phrase(unsigned model);
pogwake_engine_t *pogwake_engine_create(unsigned model);
/* 16 kHz mono PCM. -1 = failure, 0 = waiting, 1 = wake detected. */
int pogwake_engine_feed(pogwake_engine_t *, const int16_t *, size_t);
void pogwake_engine_destroy(pogwake_engine_t *);
#ifdef __cplusplus
}
#endif
