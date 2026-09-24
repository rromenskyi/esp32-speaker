#pragma once
#include <stdbool.h>
#include "buttons.h"
#include "esp_err.h"

// Voice client: speaks docs/PROTOCOL.md to the configured server.
esp_err_t voice_start(void);
// Change the server (saved to settings) and reconnect. url NULL/"" disables.
esp_err_t voice_set_server(const char *url, const char *token);
void voice_on_button(button_t b, bool pressed);
void voice_describe(char *out, size_t len);
