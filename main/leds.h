#pragma once
#include <stdint.h>
#include "esp_err.h"

// WS2812-style addressable LEDs on one data pin (GRB, 800 kHz), driven by RMT.
esp_err_t leds_init(int gpio, int max_leds);
esp_err_t leds_set(int index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t leds_fill(int count, uint8_t r, uint8_t g, uint8_t b);   // first `count`, rest off
esp_err_t leds_show(void);
