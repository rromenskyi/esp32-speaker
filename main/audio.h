#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_MIC_SLOTS 4   // ES7210 TDM slots per frame

esp_err_t audio_init(uint32_t sample_rate);
// Queue mono 16-bit samples for playback (blocks while the queue is full).
esp_err_t audio_write_mono(const int16_t *pcm, size_t samples);
esp_err_t audio_tone(int hz, int ms, int amplitude);
// Blocking read of `frames` capture frames, AUDIO_MIC_SLOTS int16 samples each.
esp_err_t audio_read(int16_t *frames, size_t frames_count);
