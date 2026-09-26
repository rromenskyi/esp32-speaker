# speexdsp (vendored)

Subset of [xiph/speexdsp](https://github.com/xiph/speexdsp) at commit
`7a15878`: the acoustic echo canceller (`mdf.c`) and the
preprocessor (noise suppression, AGC — `preprocess.c`) with their FFT support,
and the resampler (`resample.c`) that bridges the 48 kHz bus and the 16 kHz
voice path.
Unmodified sources; license in [COPYING](COPYING) (BSD-3-Clause).

Built floating point with the bundled `smallft` FFT. `fftwrap.c` can also
build against FFTW, which is GPL — that option (`USE_GPL_FFTW3`) is never
enabled here.
