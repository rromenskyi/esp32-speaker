#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Echo cancellation + noise suppression + AGC on the mic path (speexdsp).
// Frames are AEC_FRAME samples of 16 kHz mono.
#define AEC_FRAME 320   // 20 ms

esp_err_t aec_init(void);
// mic: raw microphone, ref: what the speaker is playing (hardware loopback,
// sample-aligned with mic), out: cleaned speech. out may alias mic.
void aec_process(const int16_t *mic, const int16_t *ref, int16_t *out);
void aec_reset(void);
void aec_set_enabled(bool on);   // off = raw mic passes through
bool aec_enabled(void);
uint32_t aec_last_us(void);      // processing time of the last frame

// Level statistics (mean square per stage) for measuring echo reduction.
typedef struct { double mic, ref, cancelled, out; uint32_t frames, max_us; uint64_t echo_us; } aec_stats_t;
void aec_stats_reset(void);
aec_stats_t aec_stats_get(void);
