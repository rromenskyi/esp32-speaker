#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Station mode with the saved network; falls back to a provisioning access point
// (open, captive portal) when nothing is saved or the network can't be joined.
esp_err_t wifi_start(void);
esp_err_t wifi_save_and_connect(const char *ssid, const char *pass);
// Boot into the provisioning AP once on the next start, without joining the
// saved network (which is kept: a plain power cycle afterwards rejoins it).
esp_err_t wifi_portal_next_boot(void);
bool wifi_connected(void);
bool wifi_provisioning(void);
const char *wifi_hostname(void);          // e.g. "esp32-speaker-b2add8"
void wifi_status(char *out, size_t len);  // one-line human-readable status
int wifi_rssi(void);

// JSON array of visible networks: [{"ssid":"..","rssi":-50,"auth":3},...]
int wifi_scan_json(char *out, size_t len);
