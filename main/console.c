// Bring-up console on the USB-Serial/JTAG port: poke I2C registers, play tones,
// toggle expander pins — iterate on the hardware without reflashing.
#include "console.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "es7210.h"
#include "esp_heap_caps.h"
#include "audio.h"
#include "board.h"
#include "es8311.h"
#include "esp_console.h"
#include "i2c_bus.h"
#include "leds.h"
#include "status.h"
#include "voice.h"
#include "settings.h"
#include "tca9555.h"
#include "wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// --- microphone bring-up -------------------------------------------------

#define REC_MAX_MS 5000
static int16_t *s_rec;        // mono recording of one slot, PSRAM
static size_t s_rec_len;

// Capture `ms` of audio, print RMS/peak per TDM slot. The first 20 ms are
// discarded: RX runs unattended between commands and its DMA holds stale data.
static void measure(int ms)
{
    enum { CHUNK = 160 };                           // 10 ms
    static int16_t buf[CHUNK * AUDIO_MIC_SLOTS];
    double sum[AUDIO_MIC_SLOTS] = {0};
    int peak[AUDIO_MIC_SLOTS] = {0};
    size_t n = 0;
    for (int i = 0; i < 2; i++) audio_read(buf, CHUNK);
    for (int t = 0; t < ms; t += 10) {
        audio_read(buf, CHUNK);
        for (int i = 0; i < CHUNK; i++)
            for (int c = 0; c < AUDIO_MIC_SLOTS; c++) {
                int v = buf[i * AUDIO_MIC_SLOTS + c];
                sum[c] += (double)v * v;
                if (abs(v) > peak[c]) peak[c] = abs(v);
            }
        n += CHUNK;
    }
    for (int c = 0; c < AUDIO_MIC_SLOTS; c++) {
        double rms = sqrt(sum[c] / n);
        printf("  slot %d: rms %6.1f dBFS  peak %5d\n", c, rms > 0 ? 20 * log10(rms / 32768) : -120.0, peak[c]);
    }
}

static int cmd_mic(int argc, char **argv)
{
    measure(argc > 1 ? num(argv[1]) : 1000);
    return 0;
}

static int cmd_loop(int argc, char **argv)
{
    int hz = argc > 1 ? num(argv[1]) : 1000;
    audio_tone(hz, 900, 8000);        // fits in the 1 s play queue, returns at once
    vTaskDelay(pdMS_TO_TICKS(150));
    measure(600);
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    if (argc < 3) { printf("gain <mic 1..4 | -1> <dB 0..37>\n"); return 1; }
    return es7210_set_gain(num(argv[1]), num(argv[2])) == ESP_OK ? 0 : 1;
}

static int cmd_rec(int argc, char **argv)
{
    // Records all TDM slots interleaved; `tone` Hz (optional) plays meanwhile.
    int ms = argc > 1 ? num(argv[1]) : 3000, hz = argc > 2 ? num(argv[2]) : 0;
    if (ms > REC_MAX_MS) ms = REC_MAX_MS;
    if (!s_rec) s_rec = heap_caps_malloc(REC_MAX_MS * 16 * 2 * AUDIO_MIC_SLOTS, MALLOC_CAP_SPIRAM);
    if (!s_rec) return 1;
    enum { CHUNK = 160 };
    static int16_t buf[CHUNK * AUDIO_MIC_SLOTS];
    if (hz) { audio_tone(hz, ms < 900 ? ms + 100 : 900, 8000); vTaskDelay(pdMS_TO_TICKS(100)); }
    for (int i = 0; i < 2; i++) audio_read(buf, CHUNK);
    s_rec_len = 0;
    printf("recording %d ms...\n", ms);
    for (int t = 0; t < ms; t += 10) {
        audio_read(buf, CHUNK);
        memcpy(&s_rec[s_rec_len * AUDIO_MIC_SLOTS], buf, sizeof(buf));
        s_rec_len += CHUNK;
    }
    printf("done, %u frames\n", (unsigned)s_rec_len);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    int slot = argc > 1 ? num(argv[1]) : 0;
    if (!s_rec_len) { printf("nothing recorded\n"); return 1; }
    // Normalize to -3 dBFS peak (max 40x) so quiet recordings are audible.
    int peak = 1;
    for (size_t i = 0; i < s_rec_len; i++) {
        int v = abs(s_rec[i * AUDIO_MIC_SLOTS + slot]);
        if (v > peak) peak = v;
    }
    float g = 23000.0f / peak;
    if (g > 40) g = 40;
    if (g < 1) g = 1;
    printf("peak %d, playback gain x%.1f\n", peak, g);
    int16_t mono[160];
    for (size_t i = 0; i < s_rec_len; i += 160) {
        size_t n = s_rec_len - i < 160 ? s_rec_len - i : 160;
        for (size_t k = 0; k < n; k++) mono[k] = (int16_t)(s_rec[(i + k) * AUDIO_MIC_SLOTS + slot] * g);
        audio_write_mono(mono, n);
    }
    return 0;
}

// Hex dump of the interleaved recording, for offline analysis.
static int cmd_dump(int argc, char **argv)
{
    size_t frames = argc > 1 ? num(argv[1]) : s_rec_len;
    if (frames > s_rec_len) frames = s_rec_len;
    printf("DUMP %u\n", (unsigned)frames);
    const uint16_t *u = (const uint16_t *)s_rec;
    for (size_t i = 0; i < frames * AUDIO_MIC_SLOTS; i++) {
        printf("%04x", u[i]);
        if (i % 32 == 31) printf("\n");
    }
    printf("\nEND\n");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2) return wifi_save_and_connect(argv[1], argc > 2 ? argv[2] : "") == ESP_OK ? 0 : 1;
    char st[160];
    wifi_status(st, sizeof(st));
    printf("%s\n", st);
    return 0;
}

static int cmd_token(int argc, char **argv)
{
    // Shared secret for HTTP state-changing calls (X-Token header). "token -" clears it.
    if (argc < 2) { printf("token <value> | token -\n"); return 1; }
    return (strcmp(argv[1], "-") ? settings_set_str("token", argv[1]) : settings_erase("token")) == ESP_OK ? 0 : 1;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static int cmd_led(int argc, char **argv)
{
    // led <r> <g> <b>: every LED; led <count> <r> <g> <b>: only the first <count>.
    if (argc == 4) return leds_fill(BOARD_LED_COUNT, num(argv[1]), num(argv[2]), num(argv[3])) == ESP_OK ? 0 : 1;
    if (argc == 5) return leds_fill(num(argv[1]), num(argv[2]), num(argv[3]), num(argv[4])) == ESP_OK ? 0 : 1;
    printf("led [count] <r> <g> <b>\n");
    return 1;
}

static int cmd_status(int argc, char **argv)
{
    static const char *const names[] = {"off", "portal", "connecting", "connected", "listening",
                                        "thinking", "speaking", "error", "serverdown"};
    for (int i = 0; argc > 1 && i < 9; i++)
        if (!strcmp(argv[1], names[i])) { status_preview(i, argc > 2 ? num(argv[2]) : 8); return 0; }
    printf("status <off|portal|connecting|connected|listening|thinking|speaking|error|serverdown> [s]\n");
    return 1;
}

static int cmd_server(int argc, char **argv)
{
    // server: show; server <url> [token]: set and reconnect; server -: disable.
    if (argc >= 2)
        return voice_set_server(strcmp(argv[1], "-") ? argv[1] : NULL, argc > 2 ? argv[2] : NULL) == ESP_OK ? 0 : 1;
    char buf[256];
    voice_describe(buf, sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

static int cmd_px(int argc, char **argv)
{
    // px <index> <r> <g> <b>: light one pixel, everything else off.
    if (argc < 5) { printf("px <index> <r> <g> <b>\n"); return 1; }
    leds_fill(0, 0, 0, 0);
    leds_set(num(argv[1]), num(argv[2]), num(argv[3]), num(argv[4]));
    return leds_show() == ESP_OK ? 0 : 1;
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
        {.command = "mic",  .help = "mic [ms]: RMS/peak per TDM slot", .func = cmd_mic},
        {.command = "loop", .help = "loop [hz]: play a tone and measure every slot", .func = cmd_loop},
        {.command = "gain", .help = "gain <mic|-1> <dB>", .func = cmd_gain},
        {.command = "rec",  .help = "rec [ms] [tone_hz]: record all slots", .func = cmd_rec},
        {.command = "play", .help = "play [slot]: play one slot of the recording", .func = cmd_play},
        {.command = "dump", .help = "dump [frames]: hex dump of the recording", .func = cmd_dump},
        {.command = "wifi", .help = "wifi: status; wifi <ssid> [pass]: save and join", .func = cmd_wifi},
        {.command = "token", .help = "token <value>|-: set/clear the HTTP API token", .func = cmd_token},
        {.command = "reboot", .help = "restart the device", .func = cmd_reboot},
        {.command = "led",  .help = "led [count] <r> <g> <b>: case LEDs", .func = cmd_led},
        {.command = "status", .help = "status <state> [s]: preview an LED status pattern", .func = cmd_status},
        {.command = "server", .help = "server [<ws-url> [token] | -]: voice server", .func = cmd_server},
        {.command = "px",   .help = "px <index> <r> <g> <b>: light one pixel only", .func = cmd_px},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
