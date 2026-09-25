# TensorFlow Lite Micro audio frontend (Apache-2.0, LICENSE-tensorflow) with
# kissfft (BSD-3-Clause, kissfft/COPYING): the log-mel features microWakeWord
# models are trained on. Same sources pymicro-features builds for training, so
# device features match training features bit for bit.
set(L tensorflow/lite/experimental/microfrontend/lib)
idf_component_register(SRCS "${L}/frontend.c" "${L}/frontend_util.c" "${L}/filterbank.c" "${L}/filterbank_util.c"
                            "${L}/log_lut.c" "${L}/log_scale.c" "${L}/log_scale_util.c" "${L}/noise_reduction.c"
                            "${L}/noise_reduction_util.c" "${L}/pcan_gain_control.c" "${L}/pcan_gain_control_util.c"
                            "${L}/window.c" "${L}/window_util.c" "${L}/fft.cc" "${L}/fft_util.cc" "${L}/kiss_fft_int16.cc"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "kissfft")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-shift-negative-value)
