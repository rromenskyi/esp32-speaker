#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "wakeword.h"

// Wake word model storage in the `model` flash partition: a small header
// (magic, length, detection parameters) followed by the .tflite bytes. The
// partition is memory-mapped, so the model is used in place.
bool wwmodel_load(wakeword_config_t *cfg, char *name, size_t name_len);

typedef struct wwmodel_writer wwmodel_writer_t;
esp_err_t wwmodel_write_begin(wwmodel_writer_t **w, size_t len, float cutoff, int window, int arena, const char *name);
esp_err_t wwmodel_write(wwmodel_writer_t *w, const void *data, size_t len);
esp_err_t wwmodel_write_finish(wwmodel_writer_t *w);   // validates, then commits; frees w
void      wwmodel_write_abort(wwmodel_writer_t *w);
esp_err_t wwmodel_erase(void);   // back to the built-in model
