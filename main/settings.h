#pragma once
#include <stddef.h>
#include "esp_err.h"

// Persistent key/value settings (NVS namespace "speaker").
esp_err_t settings_init(void);
esp_err_t settings_get_str(const char *key, char *out, size_t len);   // ESP_ERR_NOT_FOUND if unset
esp_err_t settings_set_str(const char *key, const char *val);
esp_err_t settings_erase(const char *key);
