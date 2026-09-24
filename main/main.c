// esp32-speaker — boot skeleton. Prints chip/memory info so a fresh flash can be
// sanity-checked over the serial console. Real firmware lands on top of this.
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "speaker";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "esp32-speaker %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "chip rev v%d.%d, %d cores, flash %" PRIu32 " MB, PSRAM %u MB",
             chip.revision / 100, chip.revision % 100, chip.cores,
             flash_size >> 20, (unsigned)(esp_psram_get_size() >> 20));
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "free heap: internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    // Nothing here can bootloop, so confirm the image right away.
    esp_ota_mark_app_valid_cancel_rollback();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive, free internal %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    }
}
