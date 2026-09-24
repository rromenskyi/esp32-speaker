#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    BUTTON_BOOT,    // GPIO0
    BUTTON_K1,      // TCA9555 P11
    BUTTON_K2,      // TCA9555 P12
    BUTTON_K3,      // TCA9555 P13
    BUTTON_COUNT,
} button_t;

typedef void (*button_cb_t)(button_t b, bool pressed);

// Polls every button (all active low) with debouncing; logs every edge and
// calls `cb` (may be NULL) from the polling task.
esp_err_t buttons_start(button_cb_t cb);
const char *button_name(button_t b);
