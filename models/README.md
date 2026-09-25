# Wake word models

microWakeWord streaming models (TFLite, int8 input `[1, 3, 40]`) plus their
JSON metadata (`probability_cutoff`, `sliding_window_size`,
`tensor_arena_size`). The firmware currently embeds one model at build time
(`main/CMakeLists.txt`, `EMBED_FILES`; parameters in `voice_start`).

| Model | Source | License |
|-------|--------|---------|
| `okay_nabu` | [esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models) (Kevin Ahrendt) | Apache-2.0 |

`okay_nabu` is a known-good public model used to validate the detector path
until the project's own wake word model is trained (see `wakeword/`).

Test a model on the device without the microphone:
`curl --data-binary @clip.pcm http://<speaker>/wwtest` (16 kHz mono s16le);
it returns the peak probability and the number of detections.
