#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_MIC_SLOTS 4   // ES7210 TDM slots per frame

esp_err_t audio_init(uint32_t sample_rate);
// Queue mono 16-bit samples for playback (blocks while the queue is full).
esp_err_t audio_write_mono(const int16_t *pcm, size_t samples);
esp_err_t audio_write_bytes(const void *pcm16le, size_t len);
esp_err_t audio_tone(int hz, int ms, int amplitude);
void audio_play_flush(void);            // drop everything queued for playback

// Music: 48 kHz (BOARD_BUS_RATE) mono, mixed under the voice stream.
esp_err_t audio_media_write(const int16_t *pcm, size_t samples);   // blocks when full
void audio_media_flush(void);
size_t audio_media_buffered_ms(void);
void audio_set_media_gain(float gain);  // 0..1, ramped (ducking)
void audio_media_levels(float out[3]);  // music RMS now: bass, mid, treble (0..1)
bool audio_play_idle(void);
// Counters since boot: TX DMA refills that came too late (CPU starvation of
// the play task), and voice chunks that ran dry mid-stream (data too slow).
void audio_stats(uint32_t *tx_late, uint32_t *voice_gaps);             // queue empty and the last chunk played
// Which capture slots audio_read() fills (bit n = slot n; default all). The
// rest read as zeros: resampling costs CPU per slot. A slot switched back on
// replays a few ms of stale filter state.
void audio_set_capture_slots(uint32_t mask);
// Blocking read of `frames` capture frames, AUDIO_MIC_SLOTS int16 samples each.
esp_err_t audio_read(int16_t *frames, size_t frames_count);
