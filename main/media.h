#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Music player: HTTP(S) MP3 streams (internet radio, files) decoded on the
// device and mixed under the voice path.
typedef enum { MEDIA_STOPPED, MEDIA_PLAYING, MEDIA_PAUSED } media_state_t;

// Called from the media task: state is "playing", "paused", "stopped",
// "ended" or "error"; detail is the title or an error message (may be "").
typedef void (*media_cb_t)(const char *state, const char *detail);

esp_err_t media_start(media_cb_t cb);
esp_err_t media_play(const char *url, const char *title);   // replaces what's playing
void media_stop(void);
void media_pause(bool paused);
media_state_t media_state(void);
