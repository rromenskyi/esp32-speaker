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

## Pin map (from the xiaozhi board profile — to be verified on hardware)

| Function | GPIO |
|----------|------|
| I2S MCLK / BCLK / WS | 12 / 13 / 14 |
| I2S DIN (mic, from ES7210) / DOUT (to ES8311) | 15 / 16 |
| I2C SCL / SDA (codecs, IO expander, touch) | 10 / 11 |
| BOOT button | 0 |
| LED | 38 |
| LCD SPI SCLK / MOSI / CS / DC (JD9853 320x172 or ST7789 240x320) | 4 / 9 / 3 / 7 |
| LCD backlight (PWM) | 5 |
| Camera DVP (optional) | XCLK 43, PCLK 44, VSYNC 21, HREF 1, D0-D7 2,17,18,39,45,46,47,48 |

Audio chain: **ES8311** DAC (speaker, with PA) + **ES7210** 4-ch ADC (mic array,
with a hardware loopback reference channel for echo cancellation). Both codecs
share one I2S bus at the xiaozhi default 24 kHz.

### TCA9555 IO expander (I2C, address 0x20)

| Pin | Function |
|-----|----------|
| EXIO0, EXIO1 | LCD / touch reset |
| EXIO5 | camera reset |
| EXIO6 | camera power (active high) |
| EXIO8 | speaker power amplifier enable (active high) |

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
