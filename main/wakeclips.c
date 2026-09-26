#include "wakeclips.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// What the wake word detector heard before each detection: a ring of the
// last RING_SAMPLES of its input (16 kHz, after echo cancellation), copied
// out on every detection into one of MAX_CLIPS slots (newest replaces
// oldest). For finding out what sets the detector off, and for collecting
// negatives to train the next model on.
#define RING_SAMPLES (BOARD_SAMPLE_RATE * 5 / 2)   // 2.5 s
#define MAX_CLIPS    10

typedef struct {
    int64_t at_ms;      // uptime of the detection
    float p;
    char state[12];     // assistant state at the time
} clip_info_t;

static int16_t *s_ring, *s_clips;
static size_t s_pos;                 // next write index in s_ring
static clip_info_t s_info[MAX_CLIPS];
static int s_count;                  // detections since boot
static SemaphoreHandle_t s_lock;

esp_err_t wakeclips_init(void)
{
    s_ring = heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_clips = heap_caps_calloc((size_t)RING_SAMPLES * MAX_CLIPS, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    return s_ring && s_clips && s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void wakeclips_feed(const int16_t *pcm, size_t n)
{
    if (!s_ring) return;
    for (size_t i = 0; i < n; i++) {
        s_ring[s_pos] = pcm[i];
        s_pos = s_pos + 1 == RING_SAMPLES ? 0 : s_pos + 1;
    }
}

void wakeclips_capture(float p, const char *state)
{
    if (!s_ring) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = s_count % MAX_CLIPS;
    int16_t *dst = s_clips + (size_t)slot * RING_SAMPLES;
    size_t pos = s_pos;   // the mic task keeps writing; a few samples at the end may tear
    memcpy(dst, s_ring + pos, (RING_SAMPLES - pos) * sizeof(int16_t));
    memcpy(dst + (RING_SAMPLES - pos), s_ring, pos * sizeof(int16_t));
    s_info[slot] = (clip_info_t){.at_ms = esp_timer_get_time() / 1000, .p = p};
    strlcpy(s_info[slot].state, state, sizeof(s_info[slot].state));
    s_count++;
    xSemaphoreGive(s_lock);
}

// GET /wake/clips: {"count":N,"now_ms":..,"clips":[{"id":..,"at_ms":..,"p":..,"state":..}]}
// newest first; `id` is the detection number since boot.
static esp_err_t h_list(httpd_req_t *req)
{
    char buf[128];
    httpd_resp_set_type(req, "application/json");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, sizeof(buf), "{\"count\":%d,\"now_ms\":%lld,\"clips\":[", s_count, esp_timer_get_time() / 1000);
    httpd_resp_sendstr_chunk(req, buf);
    for (int k = 0; k < MAX_CLIPS && k < s_count; k++) {
        int id = s_count - 1 - k, slot = id % MAX_CLIPS;
        snprintf(buf, sizeof(buf), "%s{\"id\":%d,\"at_ms\":%lld,\"p\":%.3f,\"state\":\"%s\"}", k ? "," : "", id,
                 s_info[slot].at_ms, s_info[slot].p, s_info[slot].state);
        httpd_resp_sendstr_chunk(req, buf);
    }
    xSemaphoreGive(s_lock);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET /wake/clip?id=N: that detection's audio as a 16 kHz mono WAV.
static esp_err_t h_clip(httpd_req_t *req)
{
    char q[24], v[12];
    int id = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK)
        id = atoi(v);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = id >= 0 && id < s_count && id >= s_count - MAX_CLIPS;
    xSemaphoreGive(s_lock);
    if (!ok) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such clip (see /wake/clips)");
    const uint32_t bytes = RING_SAMPLES * 2, rate = BOARD_SAMPLE_RATE, brate = rate * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); uint32_t riff = 36 + bytes; memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8); uint32_t fmt_len = 16; memcpy(h + 16, &fmt_len, 4);
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &brate, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_send_chunk(req, (const char *)h, sizeof(h));
    // Copy out under the lock (a new detection could reuse the slot), then send.
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!copy) return httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(copy, s_clips + (size_t)(id % MAX_CLIPS) * RING_SAMPLES, bytes);
    xSemaphoreGive(s_lock);
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)copy, bytes);
    free(copy);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

void wakeclips_register(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/wake/clips", .method = HTTP_GET, .handler = h_list},
        {.uri = "/wake/clip", .method = HTTP_GET, .handler = h_clip},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
}
