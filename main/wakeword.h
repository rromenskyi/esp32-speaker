#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Streaming wake word detector for microWakeWord models (TFLite Micro, int8,
// 3 x 40 log-mel feature frames per inference, 10 ms feature step).
typedef void (*wakeword_cb_t)(float probability);

typedef struct {
    const uint8_t *model;          // .tflite flatbuffer (must stay valid)
    size_t model_len;
    float probability_cutoff;      // from the model's JSON
    int sliding_window;            // average of this many inferences
    size_t arena_size;             // tensor arena bytes (JSON value + margin)
} wakeword_config_t;

esp_err_t wakeword_start(const wakeword_config_t *cfg, wakeword_cb_t on_detect);
// Feed 16 kHz mono audio (non-blocking; drops if the detector falls behind).
void wakeword_feed(const int16_t *pcm, size_t samples);
void wakeword_set_enabled(int on);
// Full flatbuffer verification of a .tflite image (no interpreter needed).
int wakeword_model_valid(const void *model, size_t len);
float wakeword_last_probability(void);   // latest window mean, for tuning
uint32_t wakeword_last_us(void);         // last inference time
// Test mode: detections are counted and the peak probability tracked, but the
// callback isn't called (for feeding recorded clips).
void wakeword_test_begin(void);           // pauses the mic feed
void wakeword_feed_test(const int16_t *pcm, size_t samples);
void wakeword_test_end(float *peak, int *detections);

#ifdef __cplusplus
}
#endif
