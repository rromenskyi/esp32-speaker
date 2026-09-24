#pragma once
#include <stdint.h>
#include "esp_err.h"

// ES7210 4-channel mic ADC. I2S slave, 16-bit, 1xFS TDM (I2S framing):
// 4 slots per LRCK, MCLK = 256 * fs.
esp_err_t es7210_init(uint8_t addr);
esp_err_t es7210_set_gain(int mic, int db);   // mic 1..4, 0..37 dB (3 dB steps), -1 = all
