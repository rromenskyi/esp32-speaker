#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "buttons.h"
#include "esp_err.h"

// Voice client: speaks docs/PROTOCOL.md to the configured server.
esp_err_t voice_start(void);
// Change the server (saved to settings) and reconnect. url NULL/"" disables.
esp_err_t voice_set_server(const char *url, const char *token);
void voice_on_button(button_t b, bool pressed);
void voice_set_auto(bool on);   // wake word mode
void voice_set_volume(int volume);   // 0..100, saved (as the k2/k3 buttons)
int voice_volume(void);
// Send pcm (16 kHz mono) as if spoken into the mic, then play the reply as usual.
esp_err_t voice_ask_injected(const int16_t *pcm, size_t samples);
void voice_describe(char *out, size_t len);
// Capture raw TDM frames (AUDIO_MIC_SLOTS int16 each) from the mic task.
esp_err_t voice_capture_raw(int16_t *tdm, size_t frames);
// Plays noise for `ms` and leaves echo statistics in aec_stats_get().
void voice_aec_test(int ms, int amplitude);
