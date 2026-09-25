# microfrontend (vendored)

The TensorFlow Lite Micro audio frontend
(`tensorflow/lite/experimental/microfrontend/lib`, Apache-2.0 — see
LICENSE-tensorflow) and kissfft (BSD-3-Clause — see kissfft/COPYING), copied
unmodified from [pymicro-features](https://github.com/puddly/pymicro-features)
(branch `puddly/minimum-cpp-version`), which is what microWakeWord uses to
compute training features. Using the identical sources on the device keeps the
features the model sees in the field equal to the ones it was trained on.
