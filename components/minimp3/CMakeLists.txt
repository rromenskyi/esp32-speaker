# minimp3 MP3 decoder (CC0 1.0, see LICENSE), vendored unmodified.
idf_component_register(SRCS "minimp3.c" INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-unused-variable)
