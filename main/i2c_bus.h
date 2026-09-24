#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t i2c_bus_init(void);
bool      i2c_bus_probe(uint8_t addr);
esp_err_t i2c_reg_write(uint8_t addr, uint8_t reg, uint8_t val);
esp_err_t i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *val);
