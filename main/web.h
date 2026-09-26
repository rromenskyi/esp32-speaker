#pragma once
#include "esp_err.h"

// HTTP server: status page + Wi-Fi setup (also the captive portal), JSON status,
// push OTA. Optional shared token (setting "token") guards state-changing calls.
esp_err_t web_start(void);

#include <stdbool.h>
#include "esp_http_server.h"
// True if the request carries the token (or none is set); otherwise sends 401.
bool web_authorized(httpd_req_t *req);
