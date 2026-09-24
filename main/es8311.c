// ES8311 driver written from the Everest ES8311 datasheet (rev 10.0) and user
// guide (rev 1.11). Only the playback path is configured; the board's
// microphones are on the ES7210.
#include "es8311.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";

#define REG_RESET       0x00
#define REG_CLK_MGR1    0x01    // clock enables
#define REG_CLK_MGR2    0x02    // pre-divider / pre-multiplier
#define REG_CLK_ADC_OSR 0x03
#define REG_CLK_DAC_OSR 0x04
#define REG_CLK_DIV     0x05    // ADC/DAC clock dividers
#define REG_SDP_IN      0x09    // serial data port -> DAC
#define REG_SDP_OUT     0x0A    // ADC -> serial data port
#define REG_PWRUP_A     0x0B
#define REG_PWRUP_BC    0x0C
#define REG_SYS_PDN     0x0D    // analog power management
#define REG_SYS_PGA     0x0E
#define REG_SYS_BIAS    0x10
#define REG_SYS_DAC     0x12
#define REG_SYS_HP      0x13
#define REG_ADC_SCALE   0x16
#define REG_DAC_MUTE    0x31
#define REG_DAC_VOL     0x32
#define REG_DAC_RAMP    0x37
#define REG_CHIP_ID1    0xFD
#define REG_CHIP_ID2    0xFE
#define REG_CHIP_VER    0xFF

#define RESET_CSM_ON    0x80
#define RESET_ALL       0x1F

#define SDP_WL_16BIT    (3 << 2)
#define SDP_FMT_I2S     0

// DAC volume register: 0xBF = 0 dB, 0.5 dB per step. Cap at 0 dB: the board's
// class-D amplifier clips hard above that.
#define DAC_VOL_0DB     0xBF
#define DAC_VOL_RANGE   0x60    // 48 dB of usable range below 0 dB

static uint8_t s_addr;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    esp_err_t err = i2c_reg_write(s_addr, reg, val);
    if (err != ESP_OK) ESP_LOGE(TAG, "write 0x%02x=0x%02x failed: %s", reg, val, esp_err_to_name(err));
    return err;
}

esp_err_t es8311_read_id(uint8_t addr, uint8_t id[3])
{
    ESP_RETURN_ON_ERROR(i2c_reg_read(addr, REG_CHIP_ID1, &id[0]), TAG, "id1");
    ESP_RETURN_ON_ERROR(i2c_reg_read(addr, REG_CHIP_ID2, &id[1]), TAG, "id2");
    return i2c_reg_read(addr, REG_CHIP_VER, &id[2]);
}

esp_err_t es8311_init(uint8_t addr, uint32_t sample_rate)
{
    s_addr = addr;
    uint8_t id[3];
    ESP_RETURN_ON_ERROR(es8311_read_id(addr, id), TAG, "not responding at 0x%02x", addr);
    if (id[0] != 0x83 || id[1] != 0x11) {
        ESP_LOGE(TAG, "unexpected chip id %02x %02x", id[0], id[1]);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "found ES8311 rev 0x%02x at 0x%02x", id[2], addr);

    // Soft reset (user guide 9.1): assert all resets with the state machine off,
    // wait, then release. The state machine is started last.
    ESP_RETURN_ON_ERROR(wr(REG_RESET, RESET_ALL), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(wr(REG_RESET, 0x00), TAG, "reset");

    // Clocks: MCLK from the MCLK pin, every clock domain on. With MCLK = 256 fs,
    // no pre-divide/multiply and ADC/DAC dividers of 1 give the 256x internal
    // ratio the DAC needs (user guide 8.5). Single speed, 64x oversampling.
    ESP_RETURN_ON_ERROR(wr(REG_CLK_MGR1, 0x3F), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_CLK_MGR2, 0x00), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_CLK_ADC_OSR, 0x10), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_CLK_DAC_OSR, 0x10), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_CLK_DIV, 0x00), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_ADC_SCALE, 0x04), TAG, "clk");

    // Serial port: 16-bit Philips I2S both ways, left channel feeds the DAC.
    ESP_RETURN_ON_ERROR(wr(REG_SDP_IN, SDP_WL_16BIT | SDP_FMT_I2S), TAG, "sdp");
    ESP_RETURN_ON_ERROR(wr(REG_SDP_OUT, SDP_WL_16BIT | SDP_FMT_I2S), TAG, "sdp");

    // Shortest power-up stage timings, higher DAC/system bias for 3.3 V supply.
    ESP_RETURN_ON_ERROR(wr(REG_PWRUP_A, 0x00), TAG, "pwr");
    ESP_RETURN_ON_ERROR(wr(REG_PWRUP_BC, 0x00), TAG, "pwr");
    ESP_RETURN_ON_ERROR(wr(REG_SYS_BIAS, 0x1F), TAG, "pwr");

    // Start the chip state machine (slave mode).
    ESP_RETURN_ON_ERROR(wr(REG_RESET, RESET_CSM_ON), TAG, "csm");

    // Analog power up: bias, references, VMID normal-speed charge.
    ESP_RETURN_ON_ERROR(wr(REG_SYS_PDN, 0x01), TAG, "pwr");
    ESP_RETURN_ON_ERROR(wr(REG_SYS_PGA, 0x02), TAG, "pwr");
    ESP_RETURN_ON_ERROR(wr(REG_SYS_DAC, 0x00), TAG, "dac");       // DAC on
    ESP_RETURN_ON_ERROR(wr(REG_SYS_HP, 0x10), TAG, "dac");        // output driver on
    ESP_RETURN_ON_ERROR(wr(REG_DAC_RAMP, 0x08), TAG, "dac");      // no ramp, EQ bypassed
    ESP_RETURN_ON_ERROR(wr(REG_DAC_MUTE, 0x00), TAG, "dac");
    ESP_RETURN_ON_ERROR(es8311_set_volume(60), TAG, "vol");

    ESP_LOGI(TAG, "playback path up, %lu Hz", (unsigned long)sample_rate);
    return ESP_OK;
}

esp_err_t es8311_set_volume(int percent)
{
    if (percent <= 0) return wr(REG_DAC_VOL, 0x00);
    if (percent > 100) percent = 100;
    return wr(REG_DAC_VOL, DAC_VOL_0DB - DAC_VOL_RANGE + (DAC_VOL_RANGE * percent) / 100);
}
