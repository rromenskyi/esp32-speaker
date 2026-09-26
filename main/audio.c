// I2S0 in full-duplex master mode. Both directions run TDM with 4 x 16-bit slots
// (Philips framing) because they share BCLK/WS: the ES7210 needs 4 slots per
// frame, and the ES8311 (I2S slave, auto-detecting the clock ratio) simply takes
// the left channel, i.e. slot 0. MCLK = 256 fs, BCLK = 64 fs.
//
// The bus runs at BOARD_BUS_RATE (48 kHz, for music); the voice side of the
// firmware (echo canceller, wake word, the server protocol) works at 16 kHz.
// This file keeps that boundary: audio_read() delivers 16 kHz TDM frames
// (all four slots downsampled together, so mic and loopback reference stay
// sample-aligned) and the voice playback stream is 16 kHz, upsampled here.
#include "audio.h"
#include <math.h>
#include <string.h>
#include "board.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "speex/speex_resampler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define BUS_RATE        BOARD_BUS_RATE
#define RATIO           (BOARD_BUS_RATE / BOARD_SAMPLE_RATE)   // 3
#define SLOTS           AUDIO_MIC_SLOTS
#define RESAMPLE_QUALITY 4                                    // speex 0..10

// Playback goes through a PSRAM-backed stream buffer drained by a dedicated task
// that never lets the I2S DMA run dry: it writes silence when there is nothing
// to play. Letting TX underrun makes the next write land in a descriptor that is
// already playing, which is audible as a click.
#define PLAY_BUF_BYTES  (16000 * 2 * 4)      // 4 s of mono 16-bit at 16 kHz
#define PLAY_CHUNK      256                  // 16 kHz samples per round (16 ms)

// Capture: the largest audio_read() request, in 16 kHz frames.
#define READ_MAX        320

static i2s_chan_handle_t s_tx, s_rx;
static uint32_t s_rate;                 // voice rate (16 kHz)
static StreamBufferHandle_t s_play;
static volatile bool s_discard;         // set while flushing: writers drop data
static SemaphoreHandle_t s_write_lock;  // stream buffers allow only one writer at a time
static volatile bool s_playing;         // play task emitted real samples last chunk
static SpeexResamplerState *s_up;       // 16 -> 48 kHz, mono (voice playback)
static SpeexResamplerState *s_down;     // 48 -> 16 kHz, 4 interleaved slots (capture)

static void play_task(void *arg)
{
    static int16_t mono[PLAY_CHUNK];
    static int16_t up[PLAY_CHUNK * RATIO];
    static int16_t frame[PLAY_CHUNK * RATIO * SLOTS];
    uint8_t *bytes = (uint8_t *)mono;
    size_t carry = 0;   // an odd byte left over from the previous receive
    for (;;) {
        // The buffer is a byte stream and a receive may end mid-sample; keep
        // the odd byte for the next round so samples never shift.
        size_t n = carry + xStreamBufferReceive(s_play, bytes + carry, sizeof(mono) - carry, pdMS_TO_TICKS(5));
        size_t got = n / 2;
        s_playing = got > 0;
        uint8_t odd = bytes[n > 0 ? n - 1 : 0];
        // Always run a full chunk (silence-padded) through the upsampler so
        // its filter state stays continuous and the output rate constant.
        for (size_t i = got; i < PLAY_CHUNK; i++) mono[i] = 0;
        spx_uint32_t in_len = PLAY_CHUNK, out_len = PLAY_CHUNK * RATIO;
        speex_resampler_process_int(s_up, 0, mono, &in_len, up, &out_len);
        memset(frame, 0, sizeof(frame));
        for (size_t i = 0; i < out_len; i++) {
            frame[SLOTS * i] = up[i];         // left half of the frame -> ES8311 left
            frame[SLOTS * i + 2] = up[i];     // right half, in case the DAC input is switched
        }
        carry = n & 1;
        if (carry) bytes[0] = odd;
        size_t written;
        i2s_channel_write(s_tx, frame, out_len * SLOTS * sizeof(int16_t), &written, portMAX_DELAY);
    }
}

esp_err_t audio_init(uint32_t sample_rate)
{
    s_rate = sample_rate;
    ESP_RETURN_ON_FALSE(BUS_RATE % sample_rate == 0, ESP_ERR_INVALID_ARG, TAG, "bus/voice rate");
    int err;
    s_up = speex_resampler_init(1, sample_rate, BUS_RATE, RESAMPLE_QUALITY, &err);
    s_down = speex_resampler_init(SLOTS, BUS_RATE, sample_rate, RESAMPLE_QUALITY, &err);
    ESP_RETURN_ON_FALSE(s_up && s_down, ESP_ERR_NO_MEM, TAG, "resamplers");
    // Start the filters with their delay pre-filled, so the first outputs
    // aren't short (speex otherwise swallows its latency from the first calls).
    speex_resampler_skip_zeros(s_up);
    speex_resampler_skip_zeros(s_down);

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, &s_rx), TAG, "channel");

    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(BUS_RATE),   // MCLK = 256 fs
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_tx, &tdm), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &tdm), TAG, "rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");

    static StaticStreamBuffer_t sb;
    uint8_t *storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(storage, ESP_ERR_NO_MEM, TAG, "play buffer");
    s_play = xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &sb);
    s_write_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 20, NULL, 1);
    ESP_LOGI(TAG, "bus %d Hz, voice %lu Hz", BUS_RATE, (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t audio_write_mono(const int16_t *pcm, size_t samples)
{
    return audio_write_bytes(pcm, samples * 2);
}

esp_err_t audio_write_bytes(const void *data, size_t len)
{
    const uint8_t *p = data;
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    while (len && !s_discard) {
        // Short timeouts so a flush can interrupt a writer blocked on a full buffer.
        size_t n = xStreamBufferSend(s_play, p, len, pdMS_TO_TICKS(20));
        p += n;
        len -= n;
    }
    xSemaphoreGive(s_write_lock);
    return ESP_OK;
}

void audio_play_flush(void)
{
    s_discard = true;
    // Reset fails while a task is blocked on the buffer; writers give up within
    // 20 ms once s_discard is set and the reader only waits 5 ms.
    for (int i = 0; i < 50 && xStreamBufferReset(s_play) != pdPASS; i++) vTaskDelay(pdMS_TO_TICKS(5));
    s_discard = false;
}

bool audio_play_idle(void)
{
    return xStreamBufferIsEmpty(s_play) && !s_playing;
}

// Reads BUS_RATE TDM frames and hands out 16 kHz ones. The downsampler may
// return a frame more or less than in/RATIO per call; surplus frames wait in
// `pending` for the next request, so callers always get exactly what they ask.
esp_err_t audio_read(int16_t *frames, size_t frames_count)
{
    static int16_t bus[READ_MAX * RATIO * SLOTS];
    static int16_t pending[(READ_MAX + 8) * SLOTS];
    static size_t npending;
    size_t have = 0;
    while (have < frames_count) {
        size_t take = npending < frames_count - have ? npending : frames_count - have;
        if (take) {
            memcpy(frames + have * SLOTS, pending, take * SLOTS * sizeof(int16_t));
            memmove(pending, pending + take * SLOTS, (npending - take) * SLOTS * sizeof(int16_t));
            npending -= take;
            have += take;
            continue;
        }
        size_t want = frames_count - have;
        if (want > READ_MAX) want = READ_MAX;
        size_t got;
        ESP_RETURN_ON_ERROR(i2s_channel_read(s_rx, bus, want * RATIO * SLOTS * sizeof(int16_t), &got, portMAX_DELAY),
                            TAG, "read");
        spx_uint32_t in_len = got / (SLOTS * sizeof(int16_t));
        spx_uint32_t out_len = READ_MAX + 8;
        speex_resampler_process_interleaved_int(s_down, bus, &in_len, pending, &out_len);
        npending = out_len;
    }
    return ESP_OK;
}

esp_err_t audio_tone(int hz, int ms, int amplitude)
{
    int16_t buf[256];
    size_t total = (size_t)s_rate * ms / 1000, done = 0;
    size_t fade = s_rate / 100;   // 10 ms fade in/out, avoids clicks
    while (done < total) {
        size_t n = total - done < 256 ? total - done : 256;
        for (size_t i = 0; i < n; i++) {
            size_t k = done + i;
            float env = 1.0f;
            if (k < fade) env = (float)k / fade;
            else if (total - k < fade) env = (float)(total - k) / fade;
            buf[i] = (int16_t)(amplitude * env * sinf(2.0f * (float)M_PI * hz * k / s_rate));
        }
        ESP_RETURN_ON_ERROR(audio_write_mono(buf, n), TAG, "tone");
        done += n;
    }
    return ESP_OK;
}
