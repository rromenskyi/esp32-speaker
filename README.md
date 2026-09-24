# esp32-speaker

Open firmware for the ESP32-S3 smart speakers sold on AliExpress (Waveshare
ESP32-S3-AUDIO-Board internals). The goal is to make the speaker a voice
front-end for your own LLM agents: microphone array in, speech out, over Wi-Fi.

> Status: **bring-up** — speaker playback works (own ES8311 driver); microphones next.

## Hardware

See [docs/HARDWARE.md](docs/HARDWARE.md). ESP32-S3R8, 16 MB flash, 8 MB PSRAM,
ES8311 DAC + ES7210 mic array ADC, TCA9555 IO expander, optional SPI LCD.

## Build

Requires [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Back up the factory firmware before the first flash (in small chunks, see the
hardware notes):

```sh
esptool -p PORT --after no-reset read-flash 0x0 0x100000 part_00.bin
```

## CI

Every push to `main` builds the firmware and publishes a rolling `nightly`
pre-release; `v*` tags publish stable releases.

## License

MIT
