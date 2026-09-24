#include "netconsole.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "netcon";

#define PORT        23
#define LOG_BUF     8192

static StreamBufferHandle_t s_log;       // log lines waiting for the client
static SemaphoreHandle_t s_log_lock;     // stream buffers allow one writer at a time
static vprintf_like_t s_orig_vprintf;
static volatile int s_client = -1;
static TaskHandle_t s_task;

// Log hook: goes to the original sink (USB) and, without ever blocking, into a
// buffer the console task drains to the socket. Logging happens from lwip and
// Wi-Fi tasks, so sending to the socket here could deadlock.
static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_orig_vprintf(fmt, ap);
    // The console task's own logs already reach the socket through its stdout.
    if (s_client >= 0 && xTaskGetCurrentTaskHandle() != s_task) {
        char line[256];
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        if (n > 0 && xSemaphoreTake(s_log_lock, 0) == pdTRUE) {
            xStreamBufferSend(s_log, line, n, 0);
            xSemaphoreGive(s_log_lock);
        }
    }
    va_end(ap2);
    return r;
}

static int sock_write(void *cookie, const char *buf, int len)
{
    int fd = (int)(intptr_t)cookie, done = 0;
    while (done < len) {
        int n = send(fd, buf + done, len - done, 0);
        if (n <= 0) return done ? done : -1;
        done += n;
    }
    return done;
}

static void drain_log(int fd)
{
    char buf[512];
    size_t n;
    while ((n = xStreamBufferReceive(s_log, buf, sizeof(buf), 0)) > 0) sock_write((void *)(intptr_t)fd, buf, n);
}

static void serve(int fd)
{
    // Commands print through this task's own stdout (newlib stdio is per task).
    FILE *out = funopen((void *)(intptr_t)fd, NULL, sock_write, NULL, NULL);
    if (!out) return;
    setvbuf(out, NULL, _IOLBF, 256);
    FILE *saved = stdout;
    stdout = out;
    xStreamBufferReset(s_log);
    s_client = fd;
    printf("esp32-speaker console. 'help' lists commands.\r\nspk> ");
    fflush(stdout);

    char line[256];
    size_t len = 0;
    int iac = 0;                                   // telnet IAC sequence bytes to skip
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        drain_log(fd);
        if (r < 0) break;
        if (r == 0) continue;
        uint8_t c;
        if (recv(fd, &c, 1, 0) <= 0) break;
        if (iac) { iac--; continue; }
        if (c == 0xFF) { iac = 2; continue; }
        if (c == '\r' || c == '\n') {
            if (!len) continue;
            line[len] = 0;
            len = 0;
            int ret;
            esp_err_t err = esp_console_run(line, &ret);
            if (err == ESP_ERR_NOT_FOUND) printf("unknown command\r\n");
            printf("spk> ");
            fflush(stdout);
        } else if (len + 1 < sizeof(line) && c >= 0x20) {
            line[len++] = c;
        }
    }
    s_client = -1;
    stdout = saved;
    fclose(out);
}

static void netconsole_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) {
        ESP_LOGE(TAG, "listen failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "telnet console on port %d", PORT);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        serve(fd);
        close(fd);
    }
}

esp_err_t netconsole_start(void)
{
    s_log = xStreamBufferCreate(LOG_BUF, 1);
    s_log_lock = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
    return xTaskCreate(netconsole_task, "netcon", 6144, NULL, 5, &s_task) == pdPASS ? ESP_OK : ESP_FAIL;
}
