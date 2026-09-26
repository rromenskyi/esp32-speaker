#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Music player: MP3 from HTTP(S) streams (internet radio, files on a server)
// or the SD card ("/sdcard/..."), decoded on the device and mixed under the
// voice path.
typedef enum { MEDIA_STOPPED, MEDIA_PLAYING, MEDIA_PAUSED } media_state_t;

// Called from the media task: state is "playing", "paused", "stopped",
// "ended" or "error"; detail is the title or an error message (may be "").
typedef void (*media_cb_t)(const char *state, const char *detail);

esp_err_t media_start(media_cb_t cb);
esp_err_t media_play(const char *url, const char *title);   // replaces what's playing
void media_stop(void);
void media_pause(bool paused);
media_state_t media_state(void);

// Optional: called when a stream ends by itself; fill url/title and return
// true to play that next (the SD library uses it to play on).
typedef bool (*media_next_t)(const char *ended_url, char *url, size_t url_len, char *title, size_t title_len);
void media_set_next(media_next_t next);
