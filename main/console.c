// Bring-up console on the USB-Serial/JTAG port: poke I2C registers, play tones,
// toggle expander pins — iterate on the hardware without reflashing.
#include "console.h"
#include <stdio.h>
#include <stdlib.h>
#include "audio.h"
#include "board.h"
#include "es8311.h"
#include "esp_console.h"
#include "i2c_bus.h"
#include "tca9555.h"

static long num(const char *s) { return strtol(s, NULL, 0); }

static int cmd_scan(int argc, char **argv)
{
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_bus_probe(a)) printf("  0x%02x\n", a);
    return 0;
}

static int cmd_rd(int argc, char **argv)
{
    if (argc < 3) { printf("rd <addr> <reg> [count]\n"); return 1; }
    int addr = num(argv[1]), reg = num(argv[2]), cnt = argc > 3 ? num(argv[3]) : 1;
    for (int i = 0; i < cnt; i++) {
        uint8_t v;
        if (i2c_reg_read(addr, reg + i, &v) != ESP_OK) { printf("0x%02x: error\n", reg + i); return 1; }
        printf("0x%02x: 0x%02x\n", reg + i, v);
    }
    return 0;
}

static int cmd_wr(int argc, char **argv)
{
    if (argc < 4) { printf("wr <addr> <reg> <val>\n"); return 1; }
    return i2c_reg_write(num(argv[1]), num(argv[2]), num(argv[3])) == ESP_OK ? 0 : 1;
}

static int cmd_tone(int argc, char **argv)
{
    int hz = argc > 1 ? num(argv[1]) : 1000, ms = argc > 2 ? num(argv[2]) : 500;
    int amp = argc > 3 ? num(argv[3]) : 8000;
    return audio_tone(hz, ms, amp) == ESP_OK ? 0 : 1;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc < 2) { printf("vol <0..100>\n"); return 1; }
    return es8311_set_volume(num(argv[1])) == ESP_OK ? 0 : 1;
}

static int cmd_exio(int argc, char **argv)
{
    if (argc == 3) return tca9555_set_output(num(argv[1]), num(argv[2])) == ESP_OK ? 0 : 1;
    uint16_t in;
    if (tca9555_read_inputs(&in) != ESP_OK) return 1;
    printf("inputs: ");
    for (int i = 15; i >= 0; i--) printf("%d%s", (in >> i) & 1, i == 8 ? " " : "");
    printf("  (P17..P10 P07..P00)\n");
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "spk>";
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "scan", .help = "scan the I2C bus", .func = cmd_scan},
        {.command = "rd",   .help = "rd <addr> <reg> [count]", .func = cmd_rd},
        {.command = "wr",   .help = "wr <addr> <reg> <val>", .func = cmd_wr},
        {.command = "tone", .help = "tone [hz] [ms] [amplitude]", .func = cmd_tone},
        {.command = "vol",  .help = "vol <0..100>", .func = cmd_vol},
        {.command = "exio", .help = "exio: read inputs; exio <pin> <0|1>: drive output", .func = cmd_exio},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
