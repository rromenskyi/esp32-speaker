// Device side of docs/PROTOCOL.md: WebSocket link to the server, microphone
// streaming while listening, playback of server audio, push-to-talk on k1.
#include "voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aec.h"
#include "wakeword.h"
#include "wwmodel.h"
#include "audio.h"
#include "board.h"
#include "cJSON.h"
#include "es8311.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "settings.h"
#include "status.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "voice";

#define FRAME_SAMPLES     320               // 20 ms at 16 kHz
#define MAX_LISTEN_MS     15000             // hard cap on one utterance
#define THINK_TIMEOUT_MS  45000             // no "thinking" heartbeat for this long:
                                            // the server is gone, back to idle
#define MIC_SLOT          0                 // one of the two mics (slot 2 is the other)
#define REF_SLOT          1                 // hardware loopback of the speaker output

typedef enum { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING } state_t;
static const char *const ST_NAME[] = {"idle", "listening", "thinking", "speaking"};

static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t s_lock;            // guards state + client handle
static volatile state_t s_state = ST_IDLE;
static volatile bool s_linked;              // WebSocket connected and hello sent
static volatile bool s_speak_stopped;       // server sent speak stop for this turn
static int64_t s_state_since_ms;
static char s_url[160];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static volatile bool s_auto;   // wake-word mode (toggled by the boot button)
static char s_ww_name[40];
static volatile bool s_wake_listen;   // current utterance was started by the wake word
static int s_speech_ms, s_silence_ms; // VAD bookkeeping for wake-word utterances

#define WAKE_END_SILENCE_MS 700   // stop after this much silence following speech
#define WAKE_NO_SPEECH_MS   4000  // give up if no speech follows the wake word

static void set_state(state_t st)
{
    s_state = st;
    s_state_since_ms = now_ms();
    static const status_t led[] = {STATUS_OFF, STATUS_LISTENING, STATUS_THINKING, STATUS_SPEAKING};
    status_t shown = s_linked || !s_url[0] ? led[st] : STATUS_SERVER_DOWN;
    if (shown == STATUS_OFF && s_auto) shown = STATUS_AUTO_IDLE;
    status_set_voice(shown);
    ESP_LOGI(TAG, "state: %s", ST_NAME[st]);
}

static void send_json(const char *json)
{
    if (s_ws && s_linked) esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(500));
}

static void send_listen(const char *state, const char *reason)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"type\":\"listen\",\"state\":\"%s\",\"reason\":\"%s\"}", state, reason);
    send_json(buf);
}

static void start_listening(const char *reason)
{
    if (!s_linked) { status_set_voice(STATUS_ERROR); return; }
    s_wake_listen = !strcmp(reason, "wake");
    s_speech_ms = s_silence_ms = 0;
    if (s_state == ST_SPEAKING) {
        audio_play_flush();
        send_json("{\"type\":\"abort\",\"reason\":\"barge_in\"}");
    }
    send_listen("start", reason);
    set_state(ST_LISTENING);
}

static void stop_listening(const char *reason)
{
    if (s_state != ST_LISTENING) return;
    send_listen("stop", reason);
    set_state(ST_THINKING);
}

// Holding k1 from power-on for this long reboots into the Wi-Fi setup portal.
#define BOOT_HOLD_WINDOW_MS  4000     // k1 must already be down this early (buttons start ~1.3 s in)
#define BOOT_HOLD_MS         15000

static volatile bool s_boot_hold;       // k1 was down at boot and is still held

static void boot_hold_task(void *arg)
{
    int64_t t0 = now_ms();
    while (s_boot_hold) {
        int64_t held = now_ms() - t0;
        status_set_hold((int)(held * 100 / BOOT_HOLD_MS));
        if (held >= BOOT_HOLD_MS) {
            ESP_LOGW(TAG, "k1 held %d s since boot: rebooting into the Wi-Fi setup portal", BOOT_HOLD_MS / 1000);
            wifi_portal_next_boot();
            vTaskDelay(pdMS_TO_TICKS(1000));   // solid amber: "got it"
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    status_set_hold(-1);
    ESP_LOGI(TAG, "k1 released before %d s, no reset", BOOT_HOLD_MS / 1000);
    vTaskDelete(NULL);
}

#define VOLUME_STEP     10
#define VOLUME_DEFAULT  80

static int s_volume = VOLUME_DEFAULT;

static void set_volume(int v, bool beep)
{
    s_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    es8311_set_volume(s_volume);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_volume);
    settings_set_str("volume", buf);
    ESP_LOGI(TAG, "volume %d", s_volume);
    if (beep && s_state != ST_SPEAKING) audio_tone(880, 60, 8000);
}

static void on_wake(float probability)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_auto && s_linked && s_state != ST_LISTENING) {
        ESP_LOGI(TAG, "wake word (p=%.2f) while %s", probability, ST_NAME[s_state]);
        start_listening("wake");   // barges in if speaking
        audio_tone(1200, 40, 5000);
    }
    xSemaphoreGive(s_lock);
}

void voice_set_auto(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auto = on;
    settings_set_str("auto", on ? "1" : "0");
    set_state(s_state);
    xSemaphoreGive(s_lock);
}

void voice_on_button(button_t b, bool pressed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (b == BUTTON_K1 && (s_boot_hold || (pressed && now_ms() < BOOT_HOLD_WINDOW_MS))) {
        // Held since power-on: this is the Wi-Fi reset gesture, not push-to-talk.
        if (pressed && !s_boot_hold) {
            s_boot_hold = true;
            xTaskCreate(boot_hold_task, "boot_hold", 3072, NULL, 5, NULL);
        } else if (!pressed) {
            s_boot_hold = false;
        }
    } else if (b == BUTTON_K1) {
        if (pressed) start_listening("button");
        else stop_listening("button");
    } else if (b == BUTTON_BOOT) {
        // Short press toggles auto (wake word) mode; persisted.
        if (pressed) {
            s_auto = !s_auto;
            settings_set_str("auto", s_auto ? "1" : "0");
            ESP_LOGI(TAG, "auto mode %s", s_auto ? "on" : "off");
            if (s_state != ST_SPEAKING) {
                audio_tone(s_auto ? 660 : 990, 70, 7000);
                audio_tone(s_auto ? 990 : 660, 70, 7000);
            }
            set_state(s_state);   // refresh the idle LED pattern
        }
    } else if (b == BUTTON_K2 || b == BUTTON_K3) {
        // Volume -/+ is handled locally; neither edge goes to the server.
        if (pressed) set_volume(s_volume + (b == BUTTON_K3 ? VOLUME_STEP : -VOLUME_STEP), true);
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"type\":\"button\",\"name\":\"%s\",\"action\":\"%s\"}",
                 button_name(b), pressed ? "press" : "release");
        send_json(buf);
    }
    xSemaphoreGive(s_lock);
}

static void handle_text(const char *data, int len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    if (!j) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(j, "state"));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!type) {
    } else if (!strcmp(type, "hello")) {
        ESP_LOGI(TAG, "server hello, session %s", cJSON_GetStringValue(cJSON_GetObjectItem(j, "session")) ?: "-");
    } else if (!strcmp(type, "speak") && st && !strcmp(st, "start")) {
        s_speak_stopped = false;
        if (s_state == ST_LISTENING) send_listen("stop", "server");
        set_state(ST_SPEAKING);
    } else if (!strcmp(type, "speak") && st && !strcmp(st, "stop")) {
        s_speak_stopped = true;
    } else if (!strcmp(type, "listen") && st && !strcmp(st, "start")) {
        start_listening("server");
    } else if (!strcmp(type, "listen") && st && !strcmp(st, "stop")) {
        if (s_state == ST_LISTENING) set_state(ST_THINKING);
    } else if (!strcmp(type, "thinking")) {
        // Also the server's heartbeat while it works: re-entering the state
        // restarts the THINK_TIMEOUT_MS countdown.
        if (s_state != ST_SPEAKING && s_state != ST_LISTENING) set_state(ST_THINKING);
    } else if (!strcmp(type, "set")) {
        cJSON *v = cJSON_GetObjectItem(j, "volume");
        if (cJSON_IsNumber(v)) set_volume(v->valueint, false);
    } else if (!strcmp(type, "error")) {
        ESP_LOGW(TAG, "server error: %s", cJSON_GetStringValue(cJSON_GetObjectItem(j, "message")) ?: "?");
        audio_play_flush();
        set_state(ST_IDLE);
        status_set_voice(STATUS_ERROR);
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        char hello[320];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"protocol\":1,\"device\":\"%s\",\"firmware\":\"%s\","
                 "\"audio\":{\"codec\":\"pcm16\",\"rate\":%d,\"channels\":1,\"frame_ms\":20},"
                 "\"capabilities\":{\"buttons\":[\"k1\",\"k2\",\"k3\",\"boot\"],\"leds\":%d}}",
                 wifi_hostname(), esp_app_get_description()->version, BOARD_SAMPLE_RATE, BOARD_LED_COUNT);
        s_linked = true;
        send_json(hello);
        ESP_LOGI(TAG, "linked to %s", s_url);
        set_state(ST_IDLE);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_linked) ESP_LOGW(TAG, "link lost");
        s_linked = false;
        audio_play_flush();
        set_state(ST_IDLE);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x1) {
            handle_text(e->data_ptr, e->data_len);
        } else if ((e->op_code == 0x2 || e->op_code == 0x0) && s_state == ST_SPEAKING) {
            // Blocks when the playback buffer is full: that stalls the socket
            // reader, which is exactly the TCP back pressure the protocol wants.
            audio_write_bytes(e->data_ptr, e->data_len);
        }
        break;
    default:
        break;
    }
}

// Injected utterance (POST /ask): while set, the mic task streams these samples
// instead of the microphone — a remote end-to-end test of the device path.
static const int16_t *volatile s_inject;
static volatile size_t s_inject_len, s_inject_pos;

esp_err_t voice_ask_injected(const int16_t *pcm, size_t samples)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_linked || s_state == ST_LISTENING) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    s_inject_pos = 0;
    s_inject_len = samples;
    s_inject = pcm;
    start_listening("button");
    xSemaphoreGive(s_lock);
    while (s_inject) vTaskDelay(pdMS_TO_TICKS(20));   // mic task clears it after the last frame
    return ESP_OK;
}

// Raw capture tap for the console (mic/rec/loop): mic_task is the only reader
// of the I2S RX channel, so it copies raw TDM frames here on request.
static int16_t *volatile s_tap_buf;
static size_t s_tap_frames, s_tap_pos;
static SemaphoreHandle_t s_tap_done;

esp_err_t voice_capture_raw(int16_t *tdm, size_t frames)
{
    if (!s_tap_done) s_tap_done = xSemaphoreCreateBinary();
    s_tap_frames = frames;
    s_tap_pos = 0;
    s_tap_buf = tdm;
    bool ok = xSemaphoreTake(s_tap_done, pdMS_TO_TICKS(frames / 16 + 1000)) == pdTRUE;
    s_tap_buf = NULL;
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void tap_feed(const int16_t *tdm, size_t frames)
{
    int16_t *dst = s_tap_buf;
    if (!dst || s_tap_pos >= s_tap_frames) return;
    size_t n = s_tap_frames - s_tap_pos < frames ? s_tap_frames - s_tap_pos : frames;
    memcpy(dst + s_tap_pos * AUDIO_MIC_SLOTS, tdm, n * AUDIO_MIC_SLOTS * sizeof(int16_t));
    s_tap_pos += n;
    if (s_tap_pos >= s_tap_frames) xSemaphoreGive(s_tap_done);
}

// Reads the mic continuously (keeps the RX DMA fresh) and streams 20 ms frames
// while listening. Also drives the timeouts and the "speak done" report.
static void mic_task(void *arg)
{
    static int16_t tdm[FRAME_SAMPLES * AUDIO_MIC_SLOTS];
    static int16_t mono[FRAME_SAMPLES], ref[FRAME_SAMPLES], cancelled[FRAME_SAMPLES];
    for (;;) {
        audio_read(tdm, FRAME_SAMPLES);
        tap_feed(tdm, FRAME_SAMPLES);
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            mono[i] = tdm[i * AUDIO_MIC_SLOTS + MIC_SLOT];
            ref[i] = tdm[i * AUDIO_MIC_SLOTS + REF_SLOT];
        }
        // Every frame, not only while listening: the canceller keeps adapting
        // while the speaker talks, so it has converged when the user barges in.
        bool speech = aec_process(mono, ref, mono, cancelled);
        if (s_auto) wakeword_feed(cancelled, FRAME_SAMPLES);
        state_t st = s_state;
        if (s_inject && st == ST_LISTENING) {
            size_t n = s_inject_len - s_inject_pos < FRAME_SAMPLES ? s_inject_len - s_inject_pos : FRAME_SAMPLES;
            memset(mono, 0, sizeof(mono));
            memcpy(mono, s_inject + s_inject_pos, n * sizeof(int16_t));
            s_inject_pos += n;
            if (s_inject_pos >= s_inject_len) {
                esp_websocket_client_send_bin(s_ws, (const char *)mono, sizeof(mono), pdMS_TO_TICKS(100));
                xSemaphoreTake(s_lock, portMAX_DELAY);
                stop_listening("button");
                xSemaphoreGive(s_lock);
                s_inject = NULL;
                continue;
            }
        }
        if (st == ST_LISTENING && s_linked)
            esp_websocket_client_send_bin(s_ws, (const char *)mono, sizeof(mono), pdMS_TO_TICKS(100));
        if (st == ST_LISTENING && s_wake_listen) {
            // No button to release after a wake word: end on silence.
            if (speech) { s_speech_ms += 20; s_silence_ms = 0; }
            else s_silence_ms += 20;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // Read the state age under the lock: a button can change state between
        // frames, and a stale age once aborted fresh utterances as "timeout".
        int64_t in_state = now_ms() - s_state_since_ms;
        if (s_state == ST_LISTENING && in_state > MAX_LISTEN_MS) stop_listening("timeout");
        else if (s_state == ST_LISTENING && s_wake_listen && s_speech_ms > 200 && s_silence_ms >= WAKE_END_SILENCE_MS)
            stop_listening("silence");
        else if (s_state == ST_LISTENING && s_wake_listen && s_speech_ms <= 200 && in_state > WAKE_NO_SPEECH_MS)
            stop_listening("silence");
        else if (s_state == ST_THINKING && in_state > THINK_TIMEOUT_MS) set_state(ST_IDLE);
        else if (s_state == ST_SPEAKING && s_speak_stopped && audio_play_idle()) {
            send_json("{\"type\":\"speak\",\"state\":\"done\"}");
            set_state(ST_IDLE);
        }
        xSemaphoreGive(s_lock);
    }
}

static void link_start(void)
{
    char token[96] = "", headers[128] = "";
    if (settings_get_str("server_url", s_url, sizeof(s_url)) != ESP_OK || !s_url[0]) {
        s_url[0] = 0;
        ESP_LOGW(TAG, "no server configured (console: server <ws://host:port/path> [token])");
        return;
    }
    if (settings_get_str("server_token", token, sizeof(token)) == ESP_OK && token[0])
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", token);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = headers[0] ? headers : NULL,
        .buffer_size = 2048,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 5000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 25,
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    status_set_voice(STATUS_SERVER_DOWN);
    ESP_LOGI(TAG, "connecting to %s", s_url);
}

esp_err_t voice_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(aec_init());
    // Wake word model: an uploaded one in the `model` partition (POST
    // /wwmodel), else the built-in okay_nabu (see models/).
    extern const uint8_t ww_model_start[] asm("_binary_okay_nabu_tflite_start");
    extern const uint8_t ww_model_end[] asm("_binary_okay_nabu_tflite_end");
    wakeword_config_t ww = {
        .model = ww_model_start,
        .model_len = (size_t)(ww_model_end - ww_model_start),
        .probability_cutoff = 0.97f,
        .sliding_window = 5,
        .arena_size = 26080 + 8192,
    };
    if (!wwmodel_load(&ww, s_ww_name, sizeof(s_ww_name))) strlcpy(s_ww_name, "okay_nabu (built-in)", sizeof(s_ww_name));
    if (wakeword_start(&ww, on_wake) != ESP_OK) ESP_LOGE(TAG, "wake word detector failed to start");
    char vol[8];
    if (settings_get_str("volume", vol, sizeof(vol)) == ESP_OK) s_volume = atoi(vol);
    if (settings_get_str("auto", vol, sizeof(vol)) == ESP_OK) s_auto = vol[0] == '1';
    es8311_set_volume(s_volume);
    link_start();
    return xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 15, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_set_server(const char *url, const char *token)
{
    esp_err_t err = url && url[0] ? settings_set_str("server_url", url) : settings_erase("server_url");
    if (err == ESP_OK) err = token && token[0] ? settings_set_str("server_token", token) : settings_erase("server_token");
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ws) {
        esp_websocket_client_handle_t old = s_ws;
        s_ws = NULL;
        s_linked = false;
        xSemaphoreGive(s_lock);
        esp_websocket_client_stop(old);
        esp_websocket_client_destroy(old);
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    link_start();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void voice_describe(char *out, size_t len)
{
    snprintf(out, len, "server=%s link=%s state=%s auto=%s wakeword=%s", s_url[0] ? s_url : "(none)",
             s_linked ? "up" : "down", ST_NAME[s_state], s_auto ? "on" : "off", s_ww_name);
}

void voice_aec_test(int ms, int amplitude)
{
    // Wideband noise through the speaker; measure over the second half, once
    // the canceller has had time to converge.
    int16_t buf[FRAME_SAMPLES];
    uint32_t seed = 12345;
    int frames = ms / 20;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) * amplitude / 32768);
        }
        if (f == frames / 2) aec_stats_reset();
        audio_write_mono(buf, FRAME_SAMPLES);
    }
    while (!audio_play_idle()) vTaskDelay(pdMS_TO_TICKS(20));
}
