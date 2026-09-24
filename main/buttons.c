#include "buttons.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "tca9555.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define POLL_MS     20
#define DEBOUNCE    2       // consecutive equal samples

static const char *const NAMES[BUTTON_COUNT] = {"boot", "k1", "k2", "k3"};
static const int EXIO_PIN[BUTTON_COUNT] = {-1, 9, 10, 11};   // P11, P12, P13
static button_cb_t s_cb;

const char *button_name(button_t b) { return b < BUTTON_COUNT ? NAMES[b] : "?"; }

static void poll_task(void *arg)
{
    bool state[BUTTON_COUNT] = {0};
    int stable[BUTTON_COUNT] = {0};
    for (;;) {
        uint16_t exio = 0xFFFF;
        tca9555_read_inputs(&exio);
        for (int b = 0; b < BUTTON_COUNT; b++) {
            bool down = EXIO_PIN[b] < 0 ? gpio_get_level(BOARD_BUTTON_BOOT) == 0
                                        : !(exio & (1u << EXIO_PIN[b]));
            if (down == state[b]) { stable[b] = 0; continue; }
            if (++stable[b] < DEBOUNCE) continue;
            stable[b] = 0;
            state[b] = down;
            ESP_LOGI(TAG, "%s %s", NAMES[b], down ? "pressed" : "released");
            if (s_cb) s_cb(b, down);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t buttons_start(button_cb_t cb)
{
    s_cb = cb;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    return xTaskCreate(poll_task, "buttons", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
