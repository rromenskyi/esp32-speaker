# speexdsp (Xiph.Org, BSD-3-Clause, see COPYING): acoustic echo canceller and
# noise suppressor. Floating point (the S3 has an FPU) with the built-in
# smallft FFT — never the GPL FFTW backend.
idf_component_register(REQUIRES heap
                       SRCS "src/mdf.c" "src/preprocess.c" "src/filterbank.c" "src/fftwrap.c"
                            "src/smallft.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src" "include/speex")
target_compile_definitions(${COMPONENT_LIB} PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=
                           OVERRIDE_SPEEX_ALLOC OVERRIDE_SPEEX_REALLOC OVERRIDE_SPEEX_FREE)
# The override definitions come from this header, force-included in every source.
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/speex_alloc_psram.h")
# The S3's FPU is single precision only; speexdsp's float build calls exp/sqrt/
# pow on floats and uses double literals in per-bin loops, all of which would
# run as software doubles. tgmath.h dispatches those calls to expf/sqrtf/powf
# for float arguments, and literals become float — no source changes.
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include tgmath.h" -fsingle-precision-constant)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-variable -Wno-unused-but-set-variable
                       -Wno-maybe-uninitialized -Wno-shift-negative-value -Wno-implicit-fallthrough)
