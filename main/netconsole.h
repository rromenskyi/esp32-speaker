#pragma once
#include "esp_err.h"

// Telnet console on TCP port 23 (one client): mirrors the log and runs the same
// commands as the USB console.
esp_err_t netconsole_start(void);
