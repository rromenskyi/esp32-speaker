// I2S0 in full-duplex master mode: TX (standard Philips) drives the ES8311, RX
// will be the ES7210 TDM stream. Both share MCLK/BCLK/WS.
#include "audio.h"
#include <math.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "audio";
// Playback goes through a PSRAM-backed stream buffer drained by a dedicated task
// that never lets the I2S DMA run dry: it writes silence when there is nothing
// to play. Letting TX underrun makes the next write land in a descriptor that is
// already playing, which is audible as a click.
#define PLAY_BUF_BYTES  (16000 * 2)          // 1 s of mono 16-bit at 16 kHz
#define PLAY_CHUNK      256                  // samples per I2S write (16 ms)

static i2s_chan_handle_t s_tx, s_rx;
static uint32_t s_rate;
static StreamBufferHandle_t s_play;

static void play_task(void *arg)
{
    int16_t mono[PLAY_CHUNK];
    int16_t frame[PLAY_CHUNK * 2];
    for (;;) {
        size_t got = xStreamBufferReceive(s_play, mono, sizeof(mono), pdMS_TO_TICKS(5)) / 2;
        for (size_t i = 0; i < PLAY_CHUNK; i++)
            frame[2 * i] = frame[2 * i + 1] = i < got ? mono[i] : 0;
        size_t written;
        i2s_channel_write(s_tx, frame, sizeof(frame), &written, portMAX_DELAY);
    }
}

esp_err_t audio_init(uint32_t sample_rate)
{
    s_rate = sample_rate;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;   // underrun plays silence, not the last buffer
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, &s_rx), TAG, "channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),   // MCLK = 256 fs
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");

    static StaticStreamBuffer_t sb;
    uint8_t *storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(storage, ESP_ERR_NO_MEM, TAG, "play buffer");
    s_play = xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &sb);
    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 20, NULL, 1);
    return ESP_OK;
}

esp_err_t audio_write_mono(const int16_t *pcm, size_t samples)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = samples * 2;
    while (left) {
        size_t n = xStreamBufferSend(s_play, p, left, portMAX_DELAY);
        p += n;
        left -= n;
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
