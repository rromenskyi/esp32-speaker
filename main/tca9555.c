// TI TCA9555 16-bit I2C IO expander. Registers come in port0/port1 pairs:
// 0/1 input, 2/3 output, 4/5 polarity inversion, 6/7 configuration (1 = input).
#include "tca9555.h"
#include "i2c_bus.h"

#define REG_INPUT   0x00
#define REG_OUTPUT  0x02
#define REG_CONFIG  0x06

static uint8_t s_addr;
static uint8_t s_out[2], s_cfg[2];

esp_err_t tca9555_init(uint8_t addr)
{
    s_addr = addr;
    // Adopt the current state instead of resetting it, so pins the bootloader or a
    // previous app left alone stay untouched.
    for (int p = 0; p < 2; p++) {
        esp_err_t err = i2c_reg_read(addr, REG_OUTPUT + p, &s_out[p]);
        if (err == ESP_OK) err = i2c_reg_read(addr, REG_CONFIG + p, &s_cfg[p]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t tca9555_set_output(int pin, bool level)
{
    if (pin < 0 || pin > 15) return ESP_ERR_INVALID_ARG;
    int p = pin / 8;
    uint8_t bit = 1u << (pin % 8);
    s_out[p] = level ? (s_out[p] | bit) : (s_out[p] & ~bit);
    s_cfg[p] &= ~bit;
    // Latch the level before turning the driver on, so the pin never glitches.
    esp_err_t err = i2c_reg_write(s_addr, REG_OUTPUT + p, s_out[p]);
    return err == ESP_OK ? i2c_reg_write(s_addr, REG_CONFIG + p, s_cfg[p]) : err;
}

esp_err_t tca9555_read_inputs(uint16_t *levels)
{
    uint8_t lo, hi;
    esp_err_t err = i2c_reg_read(s_addr, REG_INPUT, &lo);
    if (err == ESP_OK) err = i2c_reg_read(s_addr, REG_INPUT + 1, &hi);
    if (err == ESP_OK) *levels = lo | (hi << 8);
    return err;
}
