#include "wwmodel.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "wwmodel";

#define MAGIC        0x4D575721u   // "!WWM"
#define HEADER_SIZE  4096          // model data starts on the next flash sector

typedef struct {
    uint32_t magic;
    uint32_t len;
    float cutoff;
    int32_t window;
    int32_t arena;
    char name[32];
} header_t;

struct wwmodel_writer {
    const esp_partition_t *part;
    header_t h;
    size_t written;
};

static const esp_partition_t *part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
}

bool wwmodel_load(wakeword_config_t *cfg, char *name, size_t name_len)
{
    const esp_partition_t *p = part();
    header_t h;
    if (!p || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK || h.magic != MAGIC ||
        h.len == 0 || h.len > p->size - HEADER_SIZE)
        return false;
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(p, HEADER_SIZE, h.len, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) return false;
    cfg->model = map;
    cfg->model_len = h.len;
    cfg->probability_cutoff = h.cutoff;
    cfg->sliding_window = h.window;
    cfg->arena_size = h.arena;
    h.name[sizeof(h.name) - 1] = 0;
    strlcpy(name, h.name, name_len);
    ESP_LOGI(TAG, "model \"%s\" from flash: %u bytes, cutoff %.2f, window %d", h.name, (unsigned)h.len, h.cutoff,
             (int)h.window);
    return true;
}

esp_err_t wwmodel_write_begin(wwmodel_writer_t **out, size_t len, float cutoff, int window, int arena, const char *name)
{
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (len == 0 || len > p->size - HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    if (!(cutoff > 0.0f && cutoff <= 1.0f) || window < 1 || window > 32 || arena < 8192 || arena > 262144)
        return ESP_ERR_INVALID_ARG;
    wwmodel_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return ESP_ERR_NO_MEM;
    w->part = p;
    w->h = (header_t){.magic = MAGIC, .len = len, .cutoff = cutoff, .window = window, .arena = arena};
    strlcpy(w->h.name, name, sizeof(w->h.name));
    // Header last, so a failed upload never leaves a valid-looking model.
    esp_err_t err = esp_partition_erase_range(p, 0, (HEADER_SIZE + len + 4095) & ~4095u);
    if (err != ESP_OK) { free(w); return err; }
    *out = w;
    return ESP_OK;
}

esp_err_t wwmodel_write(wwmodel_writer_t *w, const void *data, size_t len)
{
    if (w->written + len > w->h.len) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_partition_write(w->part, HEADER_SIZE + w->written, data, len);
    if (err == ESP_OK) w->written += len;
    return err;
}

esp_err_t wwmodel_write_finish(wwmodel_writer_t *w)
{
    // Verify the whole flatbuffer in place before writing the header that
    // makes the model live: a truncated or corrupt upload would otherwise be
    // loaded at every boot (the partition is shared by both OTA slots).
    esp_err_t err = w->written == w->h.len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        const void *map;
        esp_partition_mmap_handle_t handle;
        err = esp_partition_mmap(w->part, HEADER_SIZE, w->h.len, ESP_PARTITION_MMAP_DATA, &map, &handle);
        if (err == ESP_OK) {
            if (!wakeword_model_valid(map, w->h.len)) err = ESP_ERR_INVALID_RESPONSE;
            esp_partition_munmap(handle);
        }
    }
    if (err == ESP_OK) err = esp_partition_write(w->part, 0, &w->h, sizeof(w->h));
    if (err != ESP_OK) ESP_LOGE(TAG, "model rejected: %s", esp_err_to_name(err));
    free(w);
    return err;
}

void wwmodel_write_abort(wwmodel_writer_t *w)
{
    free(w);   // no header was written, so the partition holds no valid model
}

esp_err_t wwmodel_erase(void)
{
    const esp_partition_t *p = part();
    return p ? esp_partition_erase_range(p, 0, 4096) : ESP_ERR_NOT_FOUND;
}
