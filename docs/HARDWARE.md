# Hardware

The target is an off-the-shelf ESP32-S3 "AI speaker" sold on AliExpress. Its
factory firmware is [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) built
for the **`waveshare-s3-audio-board`** profile, i.e. the internals follow the
Waveshare [ESP32-S3-AUDIO-Board](https://www.waveshare.com/esp32-s3-audio-board.htm).

## Identified from the device (esptool)

| Item      | Value |
|-----------|-------|
| SoC       | ESP32-S3 (QFN56) rev v0.2, dual core 240 MHz, Wi-Fi + BLE 5 (no Classic BT) |
| PSRAM     | 8 MB embedded (ESP32-S3R8, octal) |
| Flash     | 16 MB quad (manufacturer 0x20, device 0x4018) |
| USB       | native USB-Serial/JTAG (`/dev/cu.usbmodem*`) |
| Crystal   | 40 MHz |

Factory partition table (for reference; this firmware uses its own `partitions.csv`):

```
nvs      data nvs     0x9000   16K
otadata  data ota     0xd000   8K
phy_init data phy     0xf000   4K
model    data spiffs  0x10000  960K   (esp-sr wake-word models)
ota_0    app  ota_0   0x100000 6M
ota_1    app  ota_1   0x700000 6M
```

## Pin map

"✅" = verified on the device with this firmware.

| Function | GPIO | |
|----------|------|---|
| I2C SCL / SDA (codecs, IO expander) | 10 / 11 | ✅ |
| I2S MCLK / BCLK / WS | 12 / 13 / 14 | ✅ |
| I2S DOUT (to ES8311, speaker) | 16 | ✅ |
| I2S DIN (from ES7210, mic array) | 15 | |
| BOOT button | 0 | |
| LED | 38 | |

There is no display on this board.

### I2C bus (100 kHz)

| Address | Device | |
|---------|--------|---|
| 0x18 | ES8311 mono codec, speaker DAC (chip id 83 11, rev 01) | ✅ |
| 0x20 | TCA9555 16-bit IO expander | ✅ |
| 0x40 | ES7210 4-channel mic ADC | ✅ (responds) |
| 0x51 | unidentified (likely an RTC) | |

### TCA9555 IO expander

| Pin | Function | |
|-----|----------|---|
| P10 (EXIO8) | speaker power amplifier enable, active high | ✅ |

At boot all inputs read high except P05.

Audio: ES8311 runs as I2S slave from the S3's MCLK (256 × fs), 16-bit Philips
I2S, 16 kHz. The ES7210 will share the same I2S bus in TDM mode.

## Flashing notes

* Reading flash over USB-Serial/JTAG is unreliable for large transfers: some
  data patterns abort the stream ("Serial data stream stopped" / "No more data
  to read"), and the failure is repeatable for the same region. Read in small
  chunks (16 KB, falling back to 4 KB / 512 B on failure) and keep the chip in
  the loader between calls (`--before no-reset --after no-reset`), otherwise the
  factory app boots between chunks (the amp pops, peripherals twitch). Each
  successful `read-flash` is MD5-checked by esptool; confirm the assembled image
  with `esptool verify-flash 0x0 dump.bin`.
* Keep a full dump of the factory flash before the first write. Vendor dumps are
  not committed to this repo.
