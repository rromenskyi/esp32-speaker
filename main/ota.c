#include "ota.h"
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "wifi.h"

static const char *TAG = "ota";

#define CONFIRM_AFTER_S   15     // Wi-Fi must be up this long
#define GIVE_UP_AFTER_S   120    // ...within this long after boot, or roll back

struct ota_job {
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    size_t written;
};

static esp_timer_handle_t s_timer;
static int s_up_s;

static void watch_cb(void *arg)
{
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    s_up_s = wifi_connected() ? s_up_s + 1 : 0;
    if (s_up_s >= CONFIRM_AFTER_S) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
        esp_timer_stop(s_timer);
    } else if (uptime_s >= GIVE_UP_AFTER_S) {
        ESP_LOGE(TAG, "new firmware never got online, rebooting to roll back");
        esp_restart();
    }
}

void ota_watch_start(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGW(TAG, "running unconfirmed firmware from %s", run->label);
    esp_timer_create_args_t t = {.callback = watch_cb, .name = "ota_watch"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_timer));
    esp_timer_start_periodic(s_timer, 1000000);
}

esp_err_t ota_job_begin(ota_job_t **out)
{
    ota_job_t *j = calloc(1, sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    j->part = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = j->part ? esp_ota_begin(j->part, OTA_SIZE_UNKNOWN, &j->handle) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) { free(j); return err; }
    ESP_LOGI(TAG, "writing to %s", j->part->label);
    *out = j;
    return ESP_OK;
}

esp_err_t ota_job_write(ota_job_t *j, const void *data, size_t len)
{
    j->written += len;
    return esp_ota_write(j->handle, data, len);
}

esp_err_t ota_job_finish(ota_job_t *j)
{
    esp_err_t err = esp_ota_end(j->handle);          // verifies the image
    if (err == ESP_OK) err = esp_ota_set_boot_partition(j->part);
    ESP_LOGI(TAG, "%u bytes: %s", (unsigned)j->written, esp_err_to_name(err));
    free(j);
    return err;
}

void ota_job_abort(ota_job_t *j)
{
    esp_ota_abort(j->handle);
    free(j);
}
