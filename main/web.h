#pragma once
#include "esp_err.h"

// HTTP server: status page + Wi-Fi setup (also the captive portal), JSON status,
// push OTA. Optional shared token (setting "token") guards state-changing calls.
esp_err_t web_start(void);
