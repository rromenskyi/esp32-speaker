# Wake word models

microWakeWord streaming models (TFLite, int8 input `[1, 3, 40]`) plus their
JSON metadata (`probability_cutoff`, `sliding_window_size`,
`tensor_arena_size`). The firmware embeds `okay_nabu` as a fallback; any other model is uploaded to
the `model` flash partition and survives app OTA updates:

```sh
curl --data-binary @model.tflite "http://<speaker>/wwmodel?cutoff=0.9&window=5&arena=45000&name=mymodel"
curl -X DELETE http://<speaker>/wwmodel      # back to the built-in model
```

The speaker reboots after either; `server` on the console shows the active
model.

| Model | Source | License |
|-------|--------|---------|
| `okay_nabu` | [esphome/micro-wake-word-models](https://github.com/esphome/micro-wake-word-models) (Kevin Ahrendt) | Apache-2.0 |

`okay_nabu` is a known-good public model used to validate the detector path
until the project's own wake word model is trained (see `wakeword/`).

Test a model on the device without the microphone:
`curl --data-binary @clip.pcm http://<speaker>/wwtest` (16 kHz mono s16le);
it returns the peak probability and the number of detections.
