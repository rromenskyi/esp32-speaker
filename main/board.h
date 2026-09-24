// Pin map of the ESP32-S3 speaker board. Verified on hardware where noted.
#pragma once
#include "driver/gpio.h"

#define BOARD_I2C_SCL       GPIO_NUM_10
#define BOARD_I2C_SDA       GPIO_NUM_11
#define BOARD_I2C_HZ        100000

#define BOARD_I2S_MCLK      GPIO_NUM_12
#define BOARD_I2S_BCLK      GPIO_NUM_13
#define BOARD_I2S_WS        GPIO_NUM_14
#define BOARD_I2S_DIN       GPIO_NUM_15   // from ES7210 (mic array)
#define BOARD_I2S_DOUT      GPIO_NUM_16   // to ES8311 (speaker)

#define BOARD_BUTTON_BOOT   GPIO_NUM_0
#define BOARD_LED           GPIO_NUM_38   // WS2812 chain data
#define BOARD_LED_COUNT     7             // individually addressable

// 7-bit I2C addresses
#define BOARD_ADDR_ES8311   0x18
#define BOARD_ADDR_ES7210   0x40
#define BOARD_ADDR_TCA9555  0x20

// TCA9555 pins (0..15, P00..P17)
#define BOARD_EXIO_PA_EN    8             // speaker power amplifier, active high

#define BOARD_SAMPLE_RATE   16000
