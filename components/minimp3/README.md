# minimp3 (vendored)

[lieff/minimp3](https://github.com/lieff/minimp3) at commit `ea99364f61c1`,
CC0 1.0 (see LICENSE). Built MP3-only without SIMD.

One local change to `include/minimp3.h`: the decode scratch in
`mp3dec_decode_frame` (~16 KB, a stack local upstream) takes a
`MINIMP3_SCRATCH_STORAGE` qualifier, which `minimp3.c` sets to
`static EXT_RAM_BSS_ATTR` so it lives in PSRAM. That makes the decoder
single-instance.
Used by `main/media.c` for internet radio and music streams.
