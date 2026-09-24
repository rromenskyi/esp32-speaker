#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_init(uint32_t sample_rate);
// Blocking write of mono 16-bit samples (duplicated to both I2S slots).
esp_err_t audio_write_mono(const int16_t *pcm, size_t samples);
esp_err_t audio_tone(int hz, int ms, int amplitude);
