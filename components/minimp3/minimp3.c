// minimp3 (CC0, github.com/lieff/minimp3) is header-only; this is its one
// implementation unit. MP3 only (no MP1/MP2); no SIMD on Xtensa.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
// The decoder's ~16 KB scratch is a stack local upstream; keep it in PSRAM
// instead (one decoder at a time in this firmware).
#include "esp_attr.h"
#define MINIMP3_SCRATCH_STORAGE static EXT_RAM_BSS_ATTR
#include "minimp3.h"
