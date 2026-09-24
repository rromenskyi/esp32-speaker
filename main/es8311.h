#pragma once
#include <stdint.h>
#include "esp_err.h"

// ES8311 mono codec, used here as the speaker DAC. I2S slave, MCLK = 256 * fs.
esp_err_t es8311_init(uint8_t addr, uint32_t sample_rate);
esp_err_t es8311_set_volume(int percent);     // 0..100, 0 = mute
esp_err_t es8311_read_id(uint8_t addr, uint8_t id[3]);
