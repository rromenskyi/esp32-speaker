#pragma once
#include <stddef.h>
#include "esp_err.h"

// Start watching a freshly OTA'd image: it is confirmed once Wi-Fi has been up
// for a while; if that doesn't happen in time the device reboots and the
// bootloader rolls back to the previous image.
void ota_watch_start(void);

// Streaming OTA writer (used by the HTTP push endpoint).
typedef struct ota_job ota_job_t;
esp_err_t ota_job_begin(ota_job_t **job);
esp_err_t ota_job_write(ota_job_t *job, const void *data, size_t len);
esp_err_t ota_job_finish(ota_job_t *job);   // validates and sets the boot partition
void      ota_job_abort(ota_job_t *job);
