// Route speexdsp's allocations to PSRAM: its echo/preprocess state is ~100 KB,
// which internal RAM (needed by Wi-Fi/lwIP) can't spare. Included through
// OVERRIDE_SPEEX_ALLOC hooks in os_support.h (see CMakeLists.txt).
#pragma once
#include <stdlib.h>
#include "esp_heap_caps.h"
// Set by the caller around *_init: the preprocessor's FFT runs noticeably
// faster from internal RAM, while the echo canceller's large state is fine
// in PSRAM.
extern int speex_alloc_internal;
static inline void *speex_alloc(int size)
{
    void *p = heap_caps_calloc(1, size, speex_alloc_internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, size);
}
static inline void *speex_realloc(void *ptr, int size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}
static inline void speex_free(void *ptr) { free(ptr); }
