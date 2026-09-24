#include "leds.h"
#include <stdlib.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_check.h"

static const char *TAG = "leds";

#define RES_HZ      10000000        // 0.1 us ticks

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t *s_grb;
static int s_max;

esp_err_t leds_init(int gpio, int max_leds)
{
    s_max = max_leds;
    s_grb = calloc(max_leds, 3);
    ESP_RETURN_ON_FALSE(s_grb, ESP_ERR_NO_MEM, TAG, "buffer");
    rmt_tx_channel_config_t cc = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RES_HZ,
        // DMA: the whole frame goes out without CPU refills. With a 64-symbol
        // ping-pong buffer, refill interrupts delayed by Wi-Fi load stretched a
        // low gap past the latch time and the LEDs showed shifted colors.
        .mem_block_symbols = 1024,
        .trans_queue_depth = 2,
        .flags.with_dma = true,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cc, &s_chan), TAG, "channel");
    // WS2812 bit timing: 0 = 0.3 us high / 0.9 us low, 1 = 0.9 us high / 0.3 us low.
    rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&ec, &s_enc), TAG, "encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "enable");
    return leds_show();
}

esp_err_t leds_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= s_max) return ESP_ERR_INVALID_ARG;
    // These LEDs take RGB byte order (not the usual WS2812 GRB): verified by
    // sending pure green and seeing red.
    s_grb[3 * i] = r;
    s_grb[3 * i + 1] = g;
    s_grb[3 * i + 2] = b;
    return ESP_OK;
}

esp_err_t leds_fill(int count, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < s_max; i++) {
        if (i < count) leds_set(i, r, g, b);
        else leds_set(i, 0, 0, 0);
    }
    return leds_show();
}

esp_err_t leds_show(void)
{
    rmt_transmit_config_t tc = {0};
    ESP_RETURN_ON_ERROR(rmt_transmit(s_chan, s_enc, s_grb, s_max * 3, &tc), TAG, "tx");
    // The line idles low after the frame; >50 us of low latches the colors.
    return rmt_tx_wait_all_done(s_chan, 100);
}
