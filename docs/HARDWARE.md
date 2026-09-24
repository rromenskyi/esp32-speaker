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
| I2S DIN (from ES7210, mic array) | 15 | ✅ |
| BOOT button (4th button on the case) | 0 | ✅ |
| Case LEDs, WS2812 data | 38 | ✅ |

There is no display on this board.

### I2C bus (100 kHz)

| Address | Device | |
|---------|--------|---|
| 0x18 | ES8311 mono codec, speaker DAC (chip id 83 11, rev 01) | ✅ |
| 0x20 | TCA9555 16-bit IO expander | ✅ |
| 0x40 | ES7210 4-channel mic ADC (chip id 72 10) | ✅ |
| 0x51 | unidentified (likely an RTC) | |

### TCA9555 IO expander

| Pin | Function | |
|-----|----------|---|
| P10 (EXIO8) | speaker power amplifier enable, active high | ✅ |
| P11 / P12 / P13 | buttons k1 / k2 / k3, active low | ✅ |

At boot all inputs read high except P05.

### Buttons and switches

On the case, left to right: **k1, k2, k3, boot, reset**, plus a battery power
slide switch. k1–k3 are on the TCA9555 (P11–P13), boot is GPIO0 (also the ROM
download strap), reset is the chip's EN pin (not visible to firmware). All
buttons are active low; `main/buttons.c` polls them every 20 ms with debouncing.

### LEDs

The case has **7 individually addressable WS2812-type RGB LEDs** in one chain on
GPIO38 (GRB byte order, 800 kHz), driven by RMT in `main/leds.c`. A small green
LED next to the USB connector is not software-controlled (likely the charger's
status LED).

### Audio bus

Both codecs are I2S slaves on one bus driven by the S3: MCLK = 256 × fs,
16 kHz. The bus runs **TDM, 4 × 16-bit slots, Philips framing (BCLK = 64 × fs)
in both directions** — TX and RX share BCLK/WS, and the ES7210 needs four slots
per frame. The ES8311 auto-detects the clock ratio and plays the left half of
the frame (slot 0).

Capture channels (ES7210, 1×FS TDM, measured on the device):

| Slot | Source | Idle level | With a speaker tone |
|------|--------|------------|---------------------|
| 0 | microphone | −70 dBFS | −40 dBFS |
| 1 | **speaker loopback** (hardware echo reference) | −87 dBFS | −33 dBFS, flat across frequency |
| 2 | microphone | −70 dBFS | −40 dBFS |
| 3 | not connected | −87 dBFS | −87 dBFS |

With the PGA at +37 dB, normal speech at 0.5 m peaks around −16 dBFS. The PGA
scales linearly in 3 dB steps (verified 0 / 15 / 37 dB).

## Flashing notes

* Reading flash over USB-Serial/JTAG is unreliable for large transfers: some
  data patterns abort the stream ("Serial data stream stopped" / "No more data
  to read"), and the failure is repeatable for the same region. Read in small
  chunks (16 KB, falling back to 4 KB / 512 B on failure) and keep the chip in
  the loader between calls (`--before no-reset --after no-reset`), otherwise the
  factory app boots between chunks (the amp pops, peripherals twitch). Each
  successful `read-flash` is MD5-checked by esptool; confirm the assembled image
  with `esptool verify-flash 0x0 dump.bin`.
* **Don't let a reset follow a reset on USB-Serial/JTAG.** On macOS, opening the
  port toggles DTR/RTS (= reset/boot straps). esptool's RTS hard reset followed
  by a port open can wedge the USB bridge until a power cycle ("No serial data
  received"). Flash with `--after watchdog-reset`, wait a few seconds, then
  open a monitor once and keep it open.
* Keep a full dump of the factory flash before the first write. Vendor dumps are
  not committed to this repo.
