#include "status.h"
#include <math.h>
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
