# esp32-speaker

Open firmware for the ESP32-S3 smart speakers sold on AliExpress (Waveshare
ESP32-S3-AUDIO-Board internals). The goal is to make the speaker a voice
front-end for your own LLM agents: microphone array in, speech out, over Wi-Fi.

> Status: **bring-up** — speaker, mic array, buttons, Wi-Fi, push OTA with
> rollback and a telnet console work. The audio pipeline and server protocol
> are next.

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

## First setup

With no saved network the speaker opens an access point
`esp32-speaker-XXXXXX` (open, captive portal at `http://192.168.4.1`): pick
your 2.4 GHz network and enter the password. Alternatively, on the USB console:
`wifi <ssid> <password>`.

Once joined it is reachable as `esp32-speaker-XXXXXX.local`:

| | |
|---|---|
| `http://<host>/` | status and Wi-Fi setup |
| `GET /status` | JSON status (version, uptime, Wi-Fi, heap, OTA state) |
| `POST /ota` | push a firmware image: `curl --data-binary @build/esp32_speaker.bin http://<host>/ota` |
| `POST /reboot` | restart |
| `telnet <host>` | console (same commands as USB) with the live log |

Setting a token (`token <secret>` on the console) makes `/ota`, `/wifi` and
`/reboot` require an `X-Token: <secret>` header. Without it the device trusts
its LAN — don't expose it to the internet.

A pushed image boots unconfirmed: it is kept only once Wi-Fi has been up for
15 s. If that doesn't happen within 2 minutes the speaker reboots and the
bootloader rolls back to the previous image.

## CI

Every push to `main` builds the firmware and publishes a rolling `nightly`
pre-release; `v*` tags publish stable releases.

## License

MIT
