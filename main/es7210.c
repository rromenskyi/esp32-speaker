// ES7210 driver written from the Everest ES7210 user guide (2018-06-07). The init
// order follows the guide's "TDMIN-IIS" reference sequence (section 5.1); the
// clock register is derived for MCLK = 256 fs (section 4.7).
#include "es7210.h"
#include "i2c_bus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es7210";

#define REG_RESET       0x00
#define REG_CLK_DIV     0x02    // DLL_BYPASS | CLKDBL_VALID | CLK_ADC_DIV
#define REG_POWER       0x06
#define REG_OSR         0x07
#define REG_CHIPINI     0x09
#define REG_PWRUP       0x0A
#define REG_SDP         0x11    // word length / format
#define REG_SDP_MODE    0x12    // SDOUT mode (TDM)
#define REG_ADC34_HPF2  0x20
#define REG_ADC34_HPF1  0x21
#define REG_ADC12_HPF2  0x22
#define REG_ADC12_HPF1  0x23
#define REG_CHIP_ID1    0x3D
#define REG_CHIP_ID0    0x3E
#define REG_ANALOG      0x40
#define REG_MICBIAS12   0x41
#define REG_MICBIAS34   0x42
#define REG_MIC1_GAIN   0x43    // .. 0x46: SELMIC (bit 4) | gain (3:0)
#define REG_MIC1_LP     0x47    // .. 0x4A
#define REG_PDN12       0x4B
#define REG_PDN34       0x4C

#define SDP_WL_16BIT        (3 << 5)
#define SDP_FMT_I2S         0
#define SDOUT_TDM_1FS_I2S   0x02

// MCLK = 256 fs: divider 1, DLL bypassed, clock doubler on -> internal 512 fs,
// which the guide requires for single-speed mode with the EQ enabled.
#define CLK_256FS           0xC1
#define MIC_SELECT          0x10

static uint8_t s_addr;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    esp_err_t err = i2c_reg_write(s_addr, reg, val);
    if (err != ESP_OK) ESP_LOGE(TAG, "write 0x%02x=0x%02x failed: %s", reg, val, esp_err_to_name(err));
    return err;
}

esp_err_t es7210_init(uint8_t addr)
{
    s_addr = addr;
    uint8_t id1, id0;
    ESP_RETURN_ON_ERROR(i2c_reg_read(addr, REG_CHIP_ID1, &id1), TAG, "not responding at 0x%02x", addr);
    ESP_RETURN_ON_ERROR(i2c_reg_read(addr, REG_CHIP_ID0, &id0), TAG, "id");
    if (id1 != 0x72 || id0 != 0x10) {
        ESP_LOGE(TAG, "unexpected chip id %02x %02x", id1, id0);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "found ES7210 at 0x%02x", addr);

    ESP_RETURN_ON_ERROR(wr(REG_RESET, 0xFF), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(wr(REG_RESET, 0x32), TAG, "reset");

    ESP_RETURN_ON_ERROR(wr(REG_CHIPINI, 0x30), TAG, "timing");
    ESP_RETURN_ON_ERROR(wr(REG_PWRUP, 0x30), TAG, "timing");

    // High-pass filters on all four channels (removes the mic DC offset).
    ESP_RETURN_ON_ERROR(wr(REG_ADC12_HPF1, 0x26), TAG, "hpf");
    ESP_RETURN_ON_ERROR(wr(REG_ADC12_HPF2, 0x06), TAG, "hpf");
    ESP_RETURN_ON_ERROR(wr(REG_ADC34_HPF1, 0x26), TAG, "hpf");
    ESP_RETURN_ON_ERROR(wr(REG_ADC34_HPF2, 0x06), TAG, "hpf");

    ESP_RETURN_ON_ERROR(wr(REG_SDP, SDP_WL_16BIT | SDP_FMT_I2S), TAG, "sdp");
    ESP_RETURN_ON_ERROR(wr(REG_SDP_MODE, SDOUT_TDM_1FS_I2S), TAG, "sdp");

    // Analog: VMID, mic bias 2.87 V on both pairs.
    ESP_RETURN_ON_ERROR(wr(REG_ANALOG, 0xC3), TAG, "analog");
    ESP_RETURN_ON_ERROR(wr(REG_MICBIAS12, 0x70), TAG, "bias");
    ESP_RETURN_ON_ERROR(wr(REG_MICBIAS34, 0x70), TAG, "bias");
    ESP_RETURN_ON_ERROR(es7210_set_gain(-1, 37), TAG, "gain");   // max; voice at 0.5 m peaks ~-16 dBFS
    for (int i = 0; i < 4; i++) ESP_RETURN_ON_ERROR(wr(REG_MIC1_LP + i, 0x08), TAG, "lp");

    ESP_RETURN_ON_ERROR(wr(REG_OSR, 0x20), TAG, "osr");
    ESP_RETURN_ON_ERROR(wr(REG_CLK_DIV, CLK_256FS), TAG, "clk");
    ESP_RETURN_ON_ERROR(wr(REG_POWER, 0x04), TAG, "power");     // DLL off (bypassed)
    ESP_RETURN_ON_ERROR(wr(REG_PDN12, 0x0F), TAG, "power");     // PGAs, modulators, bias on
    ESP_RETURN_ON_ERROR(wr(REG_PDN34, 0x0F), TAG, "power");

    // Start the state machine.
    ESP_RETURN_ON_ERROR(wr(REG_RESET, 0x71), TAG, "start");
    ESP_RETURN_ON_ERROR(wr(REG_RESET, 0x41), TAG, "start");
    ESP_LOGI(TAG, "capture path up (4 ch TDM)");
    return ESP_OK;
}

esp_err_t es7210_set_gain(int mic, int db)
{
    if (db < 0) db = 0;
    if (db > 37) db = 37;
    uint8_t v = MIC_SELECT | (db / 3);
    for (int i = 0; i < 4; i++)
        if (mic < 0 || mic == i + 1) ESP_RETURN_ON_ERROR(wr(REG_MIC1_GAIN + i, v), TAG, "gain");
    return ESP_OK;
}
