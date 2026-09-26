// esp32-speaker — hardware bring-up: I2C scan, IO expander, ES8311 playback,
// a test tone, and a register console.
#include <inttypes.h>
#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "leds.h"
#include "status.h"
#include "voice.h"
#include "console.h"
#include "es7210.h"
#include "es8311.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "i2c_bus.h"
#include "netconsole.h"
#include "ota.h"
#include "settings.h"
#include "tca9555.h"
#include "web.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "speaker";

static void log_system(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "esp32-speaker %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "chip rev v%d.%d, flash %" PRIu32 " MB, PSRAM %u MB, free internal %u KB",
             chip.revision / 100, chip.revision % 100, flash_size >> 20,
             (unsigned)(esp_psram_get_size() >> 20),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
}

void app_main(void)
{
    log_system();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(netconsole_start());
    ota_watch_start();

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_LOGI(TAG, "I2C devices:");
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_bus_probe(a)) ESP_LOGI(TAG, "  0x%02x", a);

    if (tca9555_init(BOARD_ADDR_TCA9555) == ESP_OK) {
        uint16_t in = 0;
        tca9555_read_inputs(&in);
        ESP_LOGI(TAG, "TCA9555 inputs 0x%04x", in);
    } else {
        ESP_LOGE(TAG, "TCA9555 not found at 0x%02x", BOARD_ADDR_TCA9555);
    }

    // MCLK must be running before the codec's clock manager is configured.
    ESP_ERROR_CHECK(audio_init(BOARD_SAMPLE_RATE));
    if (es8311_init(BOARD_ADDR_ES8311, BOARD_BUS_RATE) == ESP_OK) {
        tca9555_set_output(BOARD_EXIO_PA_EN, true);
        vTaskDelay(pdMS_TO_TICKS(50));
        audio_tone(880, 150, 6000);
        audio_tone(1320, 150, 6000);
    }
    es7210_init(BOARD_ADDR_ES7210);
    if (leds_init(BOARD_LED, BOARD_LED_COUNT) == ESP_OK) status_start();
    voice_start();
    buttons_start(voice_on_button);

    console_start();
}
