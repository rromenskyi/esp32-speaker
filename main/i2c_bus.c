// Shared I2C master bus for the codecs and the IO expander. Devices are addressed
// ad hoc (register read/write by 7-bit address); handles are cached per address.
#include "i2c_bus.h"
#include "board.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TIMEOUT_MS 50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev[128];
static SemaphoreHandle_t s_lock;

esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    s_lock = xSemaphoreCreateMutex();
    return i2c_new_master_bus(&cfg, &s_bus);
}

bool i2c_bus_probe(uint8_t addr)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = i2c_master_probe(s_bus, addr, TIMEOUT_MS) == ESP_OK;
    xSemaphoreGive(s_lock);
    return ok;
}

static i2c_master_dev_handle_t dev(uint8_t addr)
{
    if (!s_dev[addr & 0x7f]) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev[addr & 0x7f]) != ESP_OK) return NULL;
    }
    return s_dev[addr & 0x7f];
}

esp_err_t i2c_reg_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2c_master_dev_handle_t d = dev(addr);
    esp_err_t err = d ? i2c_master_transmit(d, buf, 2, TIMEOUT_MS) : ESP_FAIL;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_reg_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2c_master_dev_handle_t d = dev(addr);
    esp_err_t err = d ? i2c_master_transmit_receive(d, &reg, 1, val, 1, TIMEOUT_MS) : ESP_FAIL;
    xSemaphoreGive(s_lock);
    return err;
}
