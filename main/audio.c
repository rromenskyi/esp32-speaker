// I2S0 in full-duplex master mode. Both directions run TDM with 4 x 16-bit slots
// (Philips framing) because they share BCLK/WS: the ES7210 needs 4 slots per
// frame, and the ES8311 (I2S slave, auto-detecting the clock ratio) simply takes
// the left channel, i.e. slot 0. MCLK = 256 fs, BCLK = 64 fs.
#include "audio.h"
#include <math.h>
#include "board.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// Playback goes through a PSRAM-backed stream buffer drained by a dedicated task
// that never lets the I2S DMA run dry: it writes silence when there is nothing
// to play. Letting TX underrun makes the next write land in a descriptor that is
// already playing, which is audible as a click.
#define PLAY_BUF_BYTES  (16000 * 2 * 4)      // 4 s of mono 16-bit at 16 kHz
#define PLAY_CHUNK      256                  // samples per I2S write (16 ms)
#define SLOTS           AUDIO_MIC_SLOTS

static i2s_chan_handle_t s_tx, s_rx;
static uint32_t s_rate;
static StreamBufferHandle_t s_play;
static volatile bool s_discard;         // set while flushing: writers drop data
static SemaphoreHandle_t s_write_lock;  // stream buffers allow only one writer at a time
static volatile bool s_playing;         // play task emitted real samples last chunk

static void play_task(void *arg)
{
    int16_t mono[PLAY_CHUNK];
    int16_t frame[PLAY_CHUNK * SLOTS] = {0};
    uint8_t *bytes = (uint8_t *)mono;
    size_t carry = 0;   // an odd byte left over from the previous receive
    for (;;) {
        // The buffer is a byte stream and a receive may end mid-sample; keep
        // the odd byte for the next round so samples never shift.
        size_t n = carry + xStreamBufferReceive(s_play, bytes + carry, sizeof(mono) - carry, pdMS_TO_TICKS(5));
        size_t got = n / 2;
        s_playing = got > 0;
        for (size_t i = 0; i < PLAY_CHUNK; i++) {
            int16_t v = i < got ? mono[i] : 0;
            frame[SLOTS * i] = v;         // left half of the frame -> ES8311 left
            frame[SLOTS * i + 2] = v;     // right half, in case the DAC input is switched
        }
        carry = n & 1;
        if (carry) bytes[0] = bytes[n - 1];
        size_t written;
        i2s_channel_write(s_tx, frame, sizeof(frame), &written, portMAX_DELAY);
    }
}

esp_err_t audio_init(uint32_t sample_rate)
{
    s_rate = sample_rate;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, &s_rx), TAG, "channel");

    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate),   // MCLK = 256 fs
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

esp_err_t audio_read(int16_t *frames, size_t frames_count)
{
    size_t bytes = frames_count * SLOTS * sizeof(int16_t), got;
    return i2s_channel_read(s_rx, frames, bytes, &got, portMAX_DELAY);
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
