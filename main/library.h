#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Music library on the SD card (/sdcard/music/*.mp3): mounting, playback by
// track name (the library plays on in name order), and the web page + API
// under /music (list, upload, delete, play, format).
esp_err_t library_start(void);            // mounts the card; fails without one
bool library_ready(void);
esp_err_t library_play(const char *name); // "track.mp3"
void library_register(httpd_handle_t srv);
