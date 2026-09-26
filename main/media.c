#include "media.h"
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "minimp3.h"
#include "speex/speex_resampler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "media";

#define IN_BUF      (16 * 1024)      // compressed input window
#define READ_CHUNK  4096

static media_cb_t s_cb;
static media_next_t s_next;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_url[512], s_title[96];
static volatile bool s_request;       // a new URL is waiting
static volatile bool s_stop;          // abort the current stream
static volatile bool s_paused;
static volatile media_state_t s_state = MEDIA_STOPPED;

static void notify(const char *state, const char *detail)
{
    ESP_LOGI(TAG, "%s%s%s", state, detail[0] ? ": " : "", detail);
    if (s_cb) s_cb(state, detail);
}

// Decode one stream until it ends, fails or is stopped/replaced. `url` is an
// http(s) URL or a file path (the SD card's music, "/sdcard/...").
// Returns true when the stream ended by itself (not stopped, replaced or failed).
static bool play_stream(const char *url)
{
    bool is_file = url[0] == '/';
    FILE *file = NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .user_agent = "esp32-speaker",
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = is_file ? NULL : esp_http_client_init(&cfg);
    uint8_t *in = heap_caps_malloc(IN_BUF, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *mono = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME / 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t) * 2, MALLOC_CAP_SPIRAM);
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    SpeexResamplerState *rs = NULL;
    int rs_rate = 0;
    const char *failure = NULL;
    bool started = false;

    if ((!is_file && !c) || !in || !pcm || !mono || !out || !dec) { failure = "out of memory"; goto done; }
    mp3dec_init(dec);
    if (is_file) {
        if (!(file = fopen(url, "rb"))) { failure = "file not found"; goto done; }
    } else {
        if (esp_http_client_open(c, 0) != ESP_OK) { failure = "connect failed"; goto done; }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) { failure = "HTTP error"; ESP_LOGW(TAG, "HTTP %d for %s", status, url); goto done; }
    }

    size_t have = 0;
    bool eof = false;
    int empty_reads = 0;
    while (!s_stop && !s_request) {
        if (s_paused) {
            // Keep the connection; radio servers buffer a little, a long pause
            // may drop it and then the stream ends with an error.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!eof && have < IN_BUF - READ_CHUNK && file) {
            size_t n = fread(in + have, 1, READ_CHUNK, file);
            if (n == 0) eof = true;
            have += n;
        } else if (!eof && have < IN_BUF - READ_CHUNK) {
            int n = esp_http_client_read(c, (char *)in + have, READ_CHUNK);
            if (n < 0) { failure = "stream read failed"; break; }
            if (n == 0) {
                // A closed stream without a length reads 0 forever: give it
                // ~2 s, then treat it as the end.
                if (esp_http_client_is_complete_data_received(c) || ++empty_reads > 200) eof = true;
                else vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                empty_reads = 0;
            }
            have += n;
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, in, have, pcm, &info);
        if (info.frame_bytes == 0) {
            if (eof) break;             // nothing decodable left
            if (have >= IN_BUF - READ_CHUNK) have = 0;   // garbage: resync
            continue;
        }
        memmove(in, in + info.frame_bytes, have - info.frame_bytes);
        have -= info.frame_bytes;
        if (samples <= 0) continue;     // skipped (ID3, junk) — keep going

        // Down-mix to mono, resample to the bus rate.
        for (int i = 0; i < samples; i++)
            mono[i] = info.channels == 2 ? (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2) : pcm[i];
        if (info.hz != rs_rate) {
            if (rs) speex_resampler_destroy(rs);
            int err;
            rs = speex_resampler_init(1, info.hz, BOARD_BUS_RATE, 3, &err);
            if (!rs) { failure = "resampler"; break; }
            speex_resampler_skip_zeros(rs);
            rs_rate = info.hz;
            ESP_LOGI(TAG, "%d Hz, %d ch, %d kbit/s", info.hz, info.channels, info.bitrate_kbps);
        }
        spx_uint32_t in_len = samples, out_len = MINIMP3_MAX_SAMPLES_PER_FRAME * 2;
        speex_resampler_process_int(rs, 0, mono, &in_len, out, &out_len);
        if (!started) {
            started = true;
            s_state = MEDIA_PLAYING;
            notify("playing", s_title);
        }
        audio_media_write(out, out_len);   // blocks while the 2 s buffer is full
    }
done:
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (file) fclose(file);
    if (rs) speex_resampler_destroy(rs);
    free(in); free(pcm); free(mono); free(out); free(dec);
    if (s_stop || s_request) {
        audio_media_flush();
        if (s_stop && !s_request) notify("stopped", "");
    } else if (failure) {
        notify("error", failure);
    } else {
        notify("ended", s_title);
    }
    if (!s_request) s_state = MEDIA_STOPPED;
    return !s_stop && !s_request && !failure;
}

static void media_task(void *arg)
{
    char url[sizeof(s_url)];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_request) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(url, s_url, sizeof(url));
            s_request = false;
            s_stop = false;
            s_paused = false;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "play %s", url);
            if (play_stream(url) && s_next) {
                // Ended on its own: ask the library for what comes next.
                char next[sizeof(s_url)], title[sizeof(s_title)];
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (!s_request && s_next(url, next, sizeof(next), title, sizeof(title))) {
                    strlcpy(s_url, next, sizeof(s_url));
                    strlcpy(s_title, title, sizeof(s_title));
                    s_request = true;
                }
                xSemaphoreGive(s_lock);
            }
        }
    }
}

esp_err_t media_start(media_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
    // Core 0 (the mic/AEC task owns core 1); a notch below the wake word task.
    return xTaskCreatePinnedToCore(media_task, "media", 10240, NULL, 6, &s_task, 0) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t media_play(const char *url, const char *title)
{
    if (!url || (strncmp(url, "http", 4) && strncmp(url, "/sdcard/", 8))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_title, title ? title : "", sizeof(s_title));
    s_request = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void media_stop(void)
{
    if (s_state == MEDIA_STOPPED && !s_request) return;
    s_stop = true;
    s_paused = false;
}

void media_pause(bool paused)
{
    if (s_state == MEDIA_STOPPED) return;
    s_paused = paused;
    s_state = paused ? MEDIA_PAUSED : MEDIA_PLAYING;
    notify(paused ? "paused" : "playing", s_title);
}

media_state_t media_state(void) { return s_state; }

void media_set_next(media_next_t next) { s_next = next; }
