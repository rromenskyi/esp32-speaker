// Wake word detection: TFLite Micro audio frontend -> microWakeWord streaming
// model. Runs in its own task on core 0; the mic task (core 1) feeds it audio
// through a stream buffer so AEC timing is never affected.
#include "wakeword.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "wakeword";

// Feature extraction must match microWakeWord's training frontend
// (pymicro-features) exactly.
#define FEATURE_SIZE     40
#define FEATURE_SCALE    0.0390625f       // frontend uint16 -> float feature
#define STRIDE           3                // feature frames per inference
#define REFRACTORY_MS    1500             // ignore detections right after one
#define WARMUP_INFERENCES 30              // let the streaming state settle
#define FEED_BYTES       (16000 * 2 / 2)  // 0.5 s of backlog

static wakeword_config_t s_cfg;
static wakeword_cb_t s_cb;
static StreamBufferHandle_t s_audio;
static volatile int s_enabled = 1;
static volatile float s_last_prob;
static volatile uint32_t s_last_us;
static volatile bool s_test;
static volatile bool s_reset;   // detector task resets its state before the next audio
static volatile float s_test_peak;
static volatile int s_test_hits;

static tflite::MicroInterpreter *s_interp;
static FrontendState s_frontend;

static bool setup_model(void)
{
    const tflite::Model *model = tflite::GetModel(s_cfg.model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu, expected %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }
    // Exactly the ops microWakeWord streaming models use.
    static tflite::MicroMutableOpResolver<13> ops;
    ops.AddCallOnce();
    ops.AddVarHandle();
    ops.AddReadVariable();
    ops.AddAssignVariable();
    ops.AddConcatenation();
    ops.AddConv2D();
    ops.AddDepthwiseConv2D();
    ops.AddFullyConnected();
    ops.AddLogistic();
    ops.AddQuantize();
    ops.AddReshape();
    ops.AddSplitV();
    ops.AddStridedSlice();

    // Internal RAM is faster for the arena; fall back to PSRAM if short.
    uint8_t *arena = (uint8_t *)heap_caps_aligned_alloc(16, s_cfg.arena_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!arena) arena = (uint8_t *)heap_caps_aligned_alloc(16, s_cfg.arena_size, MALLOC_CAP_SPIRAM);
    if (!arena) return false;
    tflite::MicroAllocator *allocator = tflite::MicroAllocator::Create(arena, s_cfg.arena_size);
    tflite::MicroResourceVariables *vars = tflite::MicroResourceVariables::Create(allocator, 20);
    s_interp = new tflite::MicroInterpreter(model, ops, allocator, vars);
    if (s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena %u bytes too small?)", (unsigned)s_cfg.arena_size);
        return false;
    }
    TfLiteTensor *in = s_interp->input(0);
    if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[1] != STRIDE || in->dims->data[2] != FEATURE_SIZE) {
        ESP_LOGE(TAG, "unexpected model input");
        return false;
    }
    ESP_LOGI(TAG, "model ready: arena used %u of %u bytes", (unsigned)s_interp->arena_used_bytes(), (unsigned)s_cfg.arena_size);
    return true;
}

static void setup_frontend(void)
{
    FrontendConfig c;
    FrontendFillConfigWithDefaults(&c);
    c.window.size_ms = 30;
    c.window.step_size_ms = 10;
    c.filterbank.num_channels = FEATURE_SIZE;
    c.filterbank.lower_band_limit = 125.0f;
    c.filterbank.upper_band_limit = 7500.0f;
    c.noise_reduction.smoothing_bits = 10;
    c.noise_reduction.even_smoothing = 0.025f;
    c.noise_reduction.odd_smoothing = 0.06f;
    c.noise_reduction.min_signal_remaining = 0.05f;
    c.pcan_gain_control.enable_pcan = 1;
    c.pcan_gain_control.strength = 0.95f;
    c.pcan_gain_control.offset = 80.0f;
    c.pcan_gain_control.gain_bits = 21;
    c.log_scale.enable_log = 1;
    c.log_scale.scale_shift = 6;
    FrontendPopulateState(&c, &s_frontend, 16000);
}

static void detector_task(void *arg)
{
    TfLiteTensor *in = s_interp->input(0);
    TfLiteTensor *out = s_interp->output(0);
    const float in_scale = in->params.scale;
    const int in_zp = in->params.zero_point;
    int frames = 0, inferences = 0, window_pos = 0;
    float window[32] = {0};
    int win = s_cfg.sliding_window < 1 ? 1 : s_cfg.sliding_window > 32 ? 32 : s_cfg.sliding_window;
    int64_t quiet_until = 0;
    int16_t pcm[160];

    for (;;) {
        size_t got = xStreamBufferReceive(s_audio, pcm, sizeof(pcm), portMAX_DELAY) / 2;
        if (s_reset) {
            // Fresh streaming state: model variables, frontend, window, warmup.
            s_interp->Reset();
            FrontendReset(&s_frontend);
            frames = inferences = window_pos = 0;
            memset(window, 0, sizeof(window));
            s_reset = false;
        }
        const int16_t *p = pcm;
        while (got > 0) {
            size_t read = 0;
            FrontendOutput f = FrontendProcessSamples(&s_frontend, p, got, &read);
            p += read;
            got -= read;
            if (f.size != FEATURE_SIZE) continue;
            // One 10 ms feature frame -> row `frames` of the int8 input tensor.
            int8_t *row = in->data.int8 + frames * FEATURE_SIZE;
            for (int i = 0; i < FEATURE_SIZE; i++) {
                int q = (int)lroundf(f.values[i] * FEATURE_SCALE / in_scale) + in_zp;
                row[i] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
            }
            if (++frames < STRIDE) continue;
            frames = 0;

            int64_t t0 = esp_timer_get_time();
            if (s_interp->Invoke() != kTfLiteOk) {
                ESP_LOGE(TAG, "invoke failed");
                continue;
            }
            s_last_us = (uint32_t)(esp_timer_get_time() - t0);
            float prob = (out->data.uint8[0] - out->params.zero_point) * out->params.scale;
            window[window_pos] = prob;
            window_pos = (window_pos + 1) % win;
            float mean = 0;
            for (int i = 0; i < win; i++) mean += window[i];
            mean /= win;
            s_last_prob = mean;
            if (s_test && mean > s_test_peak) s_test_peak = mean;
            if (++inferences < WARMUP_INFERENCES || (!s_enabled && !s_test)) continue;
            int64_t now = esp_timer_get_time() / 1000;
            if (mean >= s_cfg.probability_cutoff && now >= quiet_until) {
                quiet_until = now + REFRACTORY_MS;
                memset(window, 0, sizeof(window));
                ESP_LOGI(TAG, "detected (p=%.3f)%s", mean, s_test ? " [test]" : "");
                if (s_test) s_test_hits = s_test_hits + 1;
                else if (s_cb) s_cb(mean);
            }
        }
    }
}

extern "C" esp_err_t wakeword_start(const wakeword_config_t *cfg, wakeword_cb_t on_detect)
{
    s_cfg = *cfg;
    s_cb = on_detect;
    setup_frontend();
    if (!setup_model()) return ESP_FAIL;
    s_audio = xStreamBufferCreate(FEED_BYTES, 320);
    return xTaskCreatePinnedToCore(detector_task, "wakeword", 6144, NULL, 8, NULL, 0) == pdPASS ? ESP_OK : ESP_FAIL;
}

extern "C" void wakeword_feed(const int16_t *pcm, size_t samples)
{
    // The mic path pauses while a test clip is being fed: two interleaved
    // streams in one buffer would be garbage to the frontend.
    if (s_audio && !s_test) xStreamBufferSend(s_audio, pcm, samples * 2, 0);
}

extern "C" void wakeword_feed_test(const int16_t *pcm, size_t samples)
{
    if (s_audio) xStreamBufferSend(s_audio, pcm, samples * 2, pdMS_TO_TICKS(100));
}

extern "C" void wakeword_set_enabled(int on) { s_enabled = on; }
extern "C" float wakeword_last_probability(void) { return s_last_prob; }
extern "C" uint32_t wakeword_last_us(void) { return s_last_us; }

extern "C" void wakeword_test_begin(void)
{
    // Each test clip starts from a clean state; otherwise the model's ~1.5 s
    // of streaming memory carries the previous clip into this one.
    s_test = true;                       // pause the mic feed first
    s_test_peak = 0;
    s_test_hits = 0;
    vTaskDelay(pdMS_TO_TICKS(40));       // let the detector drain what's queued
    xStreamBufferReset(s_audio);
    s_reset = true;
}

extern "C" void wakeword_test_end(float *peak, int *detections)
{
    vTaskDelay(pdMS_TO_TICKS(300));   // let the detector drain its backlog
    s_test = false;
    *peak = s_test_peak;
    *detections = s_test_hits;
}
