#pragma once

#include "esp_err.h"
#include "rtsp_events.h" /* METADATA_STRING_MAX */
#include <stdbool.h>
#include <stdint.h>

/**
 * Web server for control panel
 * Provides:
 * - WiFi configuration
 * - Device name configuration
 * - OTA update
 */

/**
 * Initialize and start the web server
 * @param port HTTP server port (default: 80)
 */
esp_err_t web_server_start(uint16_t port);

/**
 * Stop the web server
 */
void web_server_stop(void);

/**
 * @return true iff a device admin password is set (i.e. auth is required).
 */
bool web_server_auth_required(void);

/**
 * @param tok Candidate session token (32-hex chars)
 * @return true iff tok is a live (non-expired) session token.
 */
bool web_server_auth_token_valid(const char *tok);

/**
 * Snapshot of what is currently playing.
 *
 * Exposed because more than the web UI needs it now: the pogdev bridge reports
 * the same facts to pog Home. Copying the struct out under the caller's control
 * beats a second RTSP listener, which would duplicate the "only overwrite a
 * field this event carries" rule and let the two views drift apart.
 */
typedef struct {
  bool playing;
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t position_secs;
  uint32_t duration_secs;
} web_server_nowplaying_t;

/**
 * @param out Filled with the current now-playing snapshot. Never NULL.
 */
void web_server_get_nowplaying(web_server_nowplaying_t *out);

/**
 * Register a callback fired when the *track* changes — start, stop, new title
 * or new artist. Progress ticks do not fire it: a callback on every position
 * update would push a message a second onto whatever is listening.
 *
 * @param cb Callback, or NULL to unregister. Runs on the RTSP event task, so it
 *           must not block.
 */
void web_server_set_nowplaying_observer(void (*cb)(void));
