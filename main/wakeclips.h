#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Audio the wake word detector heard before each detection (last 10 kept in
// PSRAM), served as WAV: GET /wake/clips lists them, /wake/clip?id=N plays one.
esp_err_t wakeclips_init(void);
void wakeclips_feed(const int16_t *pcm, size_t n);        // detector input, 16 kHz
void wakeclips_capture(float p, const char *state);       // on a detection
void wakeclips_register(httpd_handle_t srv);
