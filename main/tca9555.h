#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t tca9555_init(uint8_t addr);
esp_err_t tca9555_set_output(int pin, bool level);   // also switches the pin to output
esp_err_t tca9555_read_inputs(uint16_t *levels);
