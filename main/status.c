#include "status.h"
#include <math.h>
#include "audio.h"
#include "board.h"
#include "esp_timer.h"
#include "leds.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TICK_MS          40
#define CONNECTED_SHOW_MS 2000
#define MAX_LEVEL        60      // keep it gentle; the LEDs are bright

static volatile status_t s_preview = STATUS_AUTO;
static volatile status_t s_voice = STATUS_OFF;
static volatile int s_hold = -1;              // boot-hold progress, -1 = none
static volatile int64_t s_error_until;         // ms; red error flash until then
static volatile int64_t s_preview_until;

static void fill(uint8_t r, uint8_t g, uint8_t b) { leds_fill(BOARD_LED_COUNT, r, g, b); }

// Light show while music plays and the assistant is idle: the lit length
// follows loudness, the color mixes bass (red), mids (green), treble (blue).
// Each measure has its own slow automatic gain, so quiet tracks light up too.
// Returns false (and draws nothing) when no music has sounded for a second.
static bool music_show(int64_t now)
{
    static int64_t last_sound;
    static float peak = 0.05f, band_peak[3] = {0.02f, 0.02f, 0.02f}, env;
    float b[3];
    audio_media_levels(b);
    float level = b[0] + b[1] + b[2];
    if (level > 0.002f) last_sound = now;
    if (now - last_sound > 1000) return false;
    // Gains: jump up to a new peak at once, relax over ~10 s.
    peak = level > peak ? level : fmaxf(peak * 0.996f, 0.02f);
    float k = level / peak;
    env = k > env ? k : env * 0.8f;               // fast attack, ~0.2 s release
    uint8_t c[3];
    for (int i = 0; i < 3; i++) {
        band_peak[i] = b[i] > band_peak[i] ? b[i] : fmaxf(band_peak[i] * 0.996f, 0.005f);
        float v = b[i] / band_peak[i];
        c[i] = (uint8_t)(MAX_LEVEL * v * v);       // squared: more contrast between bands
    }
    float lit = env * BOARD_LED_COUNT;
    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        float f = lit - i;                         // 1 = fully lit, 0..1 = the fading tip
        f = f > 1 ? 1 : f < 0 ? 0 : f;
        leds_set(i, (uint8_t)(c[0] * f), (uint8_t)(c[1] * f), (uint8_t)(c[2] * f));
    }
    leds_show();
    return true;
}

static void status_task(void *arg)
{
    status_t shown = STATUS_OFF, prev_auto = STATUS_OFF;
    int64_t connected_at = 0;
    uint8_t last[3] = {1, 1, 1};
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        status_t autos = wifi_provisioning() ? STATUS_PORTAL
                       : wifi_connected()    ? STATUS_CONNECTED
                                             : STATUS_CONNECTING;
        if (autos == STATUS_CONNECTED && prev_auto != STATUS_CONNECTED) connected_at = now;
        prev_auto = autos;
        if (autos == STATUS_CONNECTED && now - connected_at > CONNECTED_SHOW_MS) autos = STATUS_OFF;
        if (autos == STATUS_OFF) autos = now < s_error_until ? STATUS_ERROR : s_voice;

        shown = s_hold >= 0 ? STATUS_HOLD : autos;
        if (s_preview != STATUS_AUTO) {
            if (now < s_preview_until) shown = s_preview;
            else s_preview = STATUS_AUTO;
        }

        if ((shown == STATUS_OFF || shown == STATUS_AUTO_IDLE) && music_show(now)) {
            last[0] = last[1] = last[2] = 255;     // force a full redraw afterwards
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            continue;
        }

        uint8_t c[3] = {0, 0, 0};
        switch (shown) {
        case STATUS_PORTAL:
            if ((now / 500) % 2 == 0) c[1] = MAX_LEVEL;
            break;
        case STATUS_CONNECTING: {
            float phase = (now % 3000) / 3000.0f;            // 3 s breath
            c[2] = (uint8_t)(MAX_LEVEL * (0.5f - 0.5f * cosf(2 * (float)M_PI * phase)));
            break;
        }
        case STATUS_CONNECTED:
            c[1] = MAX_LEVEL;
            break;
        case STATUS_LISTENING:
            c[2] = MAX_LEVEL;
            break;
        case STATUS_THINKING: {
            float phase = (now % 1200) / 1200.0f;
            float k = 0.15f + 0.85f * (0.5f - 0.5f * cosf(2 * (float)M_PI * phase));
            c[0] = (uint8_t)(MAX_LEVEL * 0.6f * k);
            c[2] = (uint8_t)(MAX_LEVEL * k);
            break;
        }
        case STATUS_SPEAKING:
            c[1] = MAX_LEVEL / 3;
            break;
        case STATUS_ERROR:
            c[0] = MAX_LEVEL;
            break;
        case STATUS_HOLD: {
            int period = s_hold >= 100 ? 0 : 800 - 6 * s_hold;    // 800 ms -> 200 ms
            if (!period || (now % period) < period / 2) { c[0] = MAX_LEVEL; c[1] = MAX_LEVEL / 3; }
            break;
        }
        case STATUS_AUTO_IDLE: {
            // Faint, slow (4 s) pulse: "listening for the wake word".
            float phase = (now % 4000) / 4000.0f;
            float k = 0.5f - 0.5f * cosf(2 * (float)M_PI * phase);
            c[1] = (uint8_t)(1 + 5 * k);
            c[2] = (uint8_t)(1 + 7 * k);
            break;
        }
        case STATUS_SERVER_DOWN:
            if (now % 5000 < 150) c[0] = MAX_LEVEL / 2;
            break;
        case STATUS_OFF: {
            // Heartbeat while idle: a faint "lub-dub" every 8 s shows the
            // speaker is alive without lighting up the room.
            int64_t t = now % 8000;
            if (t < 80 || (t >= 230 && t < 310)) c[1] = 6;
            break;
        }
        default:
            break;
        }
        // Only push a frame when the color changes (keeps RMT idle otherwise).
        if (c[0] != last[0] || c[1] != last[1] || c[2] != last[2]) {
            fill(c[0], c[1], c[2]);
            last[0] = c[0]; last[1] = c[1]; last[2] = c[2];
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t status_start(void)
{
    return xTaskCreate(status_task, "status", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

void status_set_voice(status_t s) { s_voice = s; }
void status_set_hold(int progress) { s_hold = progress; }
void status_flash_error(int ms) { s_error_until = esp_timer_get_time() / 1000 + ms; }

void status_preview(status_t s, int seconds)
{
    s_preview_until = esp_timer_get_time() / 1000 + seconds * 1000LL;
    s_preview = s;
}
