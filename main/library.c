#include "library.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "media.h"
#include "sdmmc_cmd.h"
#include "tca9555.h"
#include "voice.h"
#include "web.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "library";

#define MOUNT      "/sdcard"
#define MUSIC_DIR  MOUNT "/music"
#define NAME_MAX_BYTES 120   // UTF-8 bytes of a track file name

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;        // mount/format vs. file operations
static char s_current[NAME_MAX_BYTES + 1];   // library track playing (for "next")
static esp_err_t mount(bool format_if_needed);
static esp_err_t s_mount_err = ESP_ERR_NOT_FOUND;
static int64_t s_mount_at;                  // ms; last mount attempt

// No card yet: try again, at most every 10 s (a card inserted or recovered
// after boot shows up on the next page load). Call with s_lock held.
static void ensure_mounted(void)
{
    if (!s_card && esp_timer_get_time() / 1000 - s_mount_at > 10000) mount(false);
}

// --- card -------------------------------------------------------------------

// Drop *.part files left by uploads cut short (power loss, reboot).
static void remove_partials(void)
{
    DIR *d = opendir(MUSIC_DIR);
    struct dirent *e;
    char path[sizeof(MUSIC_DIR) + 260];
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 5 && !strcmp(e->d_name + len - 5, ".part")) {
            snprintf(path, sizeof(path), MUSIC_DIR "/%s", e->d_name);
            unlink(path);
            ESP_LOGW(TAG, "removed unfinished upload %s", e->d_name);
        }
    }
    if (d) closedir(d);
}

static esp_err_t mount(bool format_if_needed)
{
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    // The card has no power switch, so a reboot in the middle of a read can
    // leave it busy; it usually answers on a second or third try.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(250));
        err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &s_card);
        if (err == ESP_OK) break;
        s_card = NULL;
    }
    s_mount_err = err;
    s_mount_at = esp_timer_get_time() / 1000;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        return err;
    }
    mkdir(MUSIC_DIR, 0777);
    remove_partials();
    ESP_LOGI(TAG, "SD card: %s, %llu MB", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

bool library_ready(void) { return s_card != NULL; }

// A track name as stored in /sdcard/music: one path component, ends in .mp3,
// and none of the characters that could break out of the page's HTML/JSON.
static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len < 5 || len > NAME_MAX_BYTES || n[0] == '.' || strcasecmp(n + len - 4, ".mp3")) return false;
    for (const char *c = n; *c; c++)
        if ((unsigned char)*c < 0x20 || strchr("/\\<>\"'&`:*?|", *c)) return false;
    return true;
}

static void track_path(char *out, size_t len, const char *name) { snprintf(out, len, MUSIC_DIR "/%s", name); }

static void title_of(char *out, size_t len, const char *name)
{
    strlcpy(out, name, len);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

esp_err_t library_play(const char *name)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_mounted();
    xSemaphoreGive(s_lock);
    if (!s_card) return ESP_ERR_NOT_FOUND;
    if (!name_ok(name)) return ESP_ERR_INVALID_ARG;
    char path[sizeof(MUSIC_DIR) + NAME_MAX_BYTES + 2], title[NAME_MAX_BYTES + 1];
    track_path(path, sizeof(path), name);
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    title_of(title, sizeof(title), name);
    strlcpy(s_current, name, sizeof(s_current));
    return media_play(path, title);
}

// Media player hook: after a library track ends, the next one in name order
// (wrapping around), so the library plays on like an album on repeat.
static bool next_track(const char *ended_url, char *url, size_t url_len, char *title, size_t title_len)
{
    if (!s_card || strncmp(ended_url, MUSIC_DIR "/", sizeof(MUSIC_DIR))) return false;
    const char *ended = ended_url + sizeof(MUSIC_DIR);
    char first[NAME_MAX_BYTES + 1] = "", next[NAME_MAX_BYTES + 1] = "";
    DIR *d = opendir(MUSIC_DIR);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!name_ok(e->d_name)) continue;
        if (!first[0] || strcmp(e->d_name, first) < 0) strlcpy(first, e->d_name, sizeof(first));
        if (strcmp(e->d_name, ended) > 0 && (!next[0] || strcmp(e->d_name, next) < 0))
            strlcpy(next, e->d_name, sizeof(next));
    }
    closedir(d);
    const char *pick = next[0] ? next : first;
    if (!pick[0]) return false;
    snprintf(url, url_len, MUSIC_DIR "/%s", pick);
    title_of(title, title_len, pick);
    strlcpy(s_current, pick, sizeof(s_current));
    return true;
}

// --- web API ------------------------------------------------------------------

static void url_decode(char *dst, const char *src, size_t len)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < len; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            dst[o++] = *p == '+' ? ' ' : *p;
        }
    }
    dst[o] = 0;
}

// ?name=... -> decoded, validated track name.
static bool query_name(httpd_req_t *req, char *name, size_t len)
{
    char q[3 * NAME_MAX_BYTES + 16], v[3 * NAME_MAX_BYTES + 8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", v, sizeof(v)) != ESP_OK) return false;
    url_decode(name, v, len);
    return name_ok(name);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t h_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char buf[NAME_MAX_BYTES + 96];
    static const char *const states[] = {"stopped", "playing", "paused"};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ensure_mounted();
    uint64_t total = 0, free_b = 0;
    if (s_card) esp_vfs_fat_info(MOUNT, &total, &free_b);
    snprintf(buf, sizeof(buf), "{\"card\":%s,\"card_error\":\"%s\",\"total\":%llu,\"free\":%llu,\"volume\":%d,\"state\":\"%s\",\"current\":\"%s\",\"tracks\":[",
             s_card ? "true" : "false", s_card ? "" : esp_err_to_name(s_mount_err), (unsigned long long)total, (unsigned long long)free_b, voice_volume(),
             states[media_state()], media_state() != MEDIA_STOPPED ? s_current : "");
    httpd_resp_sendstr_chunk(req, buf);
    DIR *d = s_card ? opendir(MUSIC_DIR) : NULL;
    bool first = true;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!name_ok(e->d_name)) continue;
        char path[sizeof(MUSIC_DIR) + NAME_MAX_BYTES + 2];
        struct stat st;
        track_path(path, sizeof(path), e->d_name);
        if (stat(path, &st) != 0) continue;
        snprintf(buf, sizeof(buf), "%s{\"name\":\"%.120s\",\"size\":%ld}", first ? "" : ",", e->d_name, (long)st.st_size);
        httpd_resp_sendstr_chunk(req, buf);
        first = false;
    }
    if (d) closedir(d);
    xSemaphoreGive(s_lock);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// POST /music/upload?name=<track.mp3>, raw file as the body. Written to a
// .part file first, so a broken upload never leaves a half track behind.
static esp_err_t h_upload(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char name[NAME_MAX_BYTES + 1], path[sizeof(MUSIC_DIR) + NAME_MAX_BYTES + 2], part[sizeof(path) + 5];
    if (!query_name(req, name, sizeof(name)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name: an .mp3 file name without / \\ < > \" ' & ` : * ? |");
    if (!s_card) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info(MOUNT, &total, &free_b);
    if ((uint64_t)req->content_len + 64 * 1024 > free_b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "card full");
    track_path(path, sizeof(path), name);
    snprintf(part, sizeof(part), "%s.part", path);
    const size_t BUF = 16 * 1024;
    char *buf = heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(part, "wb") : NULL;
    int left = req->content_len;
    bool ok = f != NULL;
    while (ok && left > 0) {
        int n = web_recv(req, buf, left < (int)BUF ? left : (int)BUF);
        if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) ok = false;
        else left -= n;
    }
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    if (!ok) {
        unlink(part);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unlink(path);   // replace an existing track of the same name
    ok = rename(part, path) == 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "uploaded %s (%d bytes)", name, req->content_len);
    return ok ? send_json(req, "{\"ok\":true}") : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
}

static esp_err_t h_delete(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char name[NAME_MAX_BYTES + 1], path[sizeof(MUSIC_DIR) + NAME_MAX_BYTES + 2];
    if (!query_name(req, name, sizeof(name))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    if (media_state() != MEDIA_STOPPED && !strcmp(name, s_current)) media_stop();
    track_path(path, sizeof(path), name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The player may still hold the file for a moment after stop.
    int rc = -1;
    for (int i = 0; i < 20 && (rc = unlink(path)) != 0; i++) vTaskDelay(pdMS_TO_TICKS(50));
    xSemaphoreGive(s_lock);
    return rc == 0 ? send_json(req, "{\"ok\":true}") : httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "delete failed");
}

// POST /music/play?name=<track.mp3> | /music/play?url=<http...>
static esp_err_t h_play(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char q[520], v[512], url[512], name[NAME_MAX_BYTES + 1];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "url", v, sizeof(v)) == ESP_OK) {
        url_decode(url, v, sizeof(url));
        s_current[0] = 0;
        return media_play(url, NULL) == ESP_OK ? send_json(req, "{\"ok\":true}")
                                               : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad url");
    }
    if (!query_name(req, name, sizeof(name))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    esp_err_t err = library_play(name);
    return err == ESP_OK ? send_json(req, "{\"ok\":true}") : httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
}

static esp_err_t h_control(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    const char *action = req->uri + strlen("/music/");
    if (!strncmp(action, "stop", 4)) media_stop();
    else if (!strncmp(action, "pause", 5)) media_pause(media_state() == MEDIA_PLAYING);
    else if (!strncmp(action, "volume", 6)) {
        char q[24], v[8];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "v", v, sizeof(v)) != ESP_OK)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "?v=0..100");
        voice_set_volume(atoi(v));
    } else if (!strncmp(action, "next", 4)) {
        char url[sizeof(MUSIC_DIR) + NAME_MAX_BYTES + 2], title[NAME_MAX_BYTES + 1], cur[sizeof(url)];
        snprintf(cur, sizeof(cur), MUSIC_DIR "/%s", s_current);
        if (next_track(cur, url, sizeof(url), title, sizeof(title))) media_play(url, title);
    }
    return send_json(req, "{\"ok\":true}");
}

// POST /music/format?confirm=yes: erase the card and make one FAT volume.
static esp_err_t h_format(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "confirm", v, sizeof(v)) != ESP_OK || strcmp(v, "yes"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "add ?confirm=yes");
    media_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_card ? esp_vfs_fat_sdcard_format(MOUNT, s_card) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) mkdir(MUSIC_DIR, 0777);
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "format: %s", esp_err_to_name(err));
    return err == ESP_OK ? send_json(req, "{\"ok\":true}") : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
}

// The page is main/music.html, embedded at build time.
extern const char music_html_start[] asm("_binary_music_html_start");
extern const char music_html_end[] asm("_binary_music_html_end");

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, music_html_start, music_html_end - music_html_start);
}

void library_register(httpd_handle_t srv)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    const httpd_uri_t uris[] = {
        {.uri = "/music", .method = HTTP_GET, .handler = h_page},
        {.uri = "/music/list", .method = HTTP_GET, .handler = h_list},
        {.uri = "/music/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/music/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/music/play", .method = HTTP_POST, .handler = h_play},
        {.uri = "/music/format", .method = HTTP_POST, .handler = h_format},
        {.uri = "/music/stop", .method = HTTP_POST, .handler = h_control},
        {.uri = "/music/pause", .method = HTTP_POST, .handler = h_control},
        {.uri = "/music/next", .method = HTTP_POST, .handler = h_control},
        {.uri = "/music/volume", .method = HTTP_POST, .handler = h_control},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
}

esp_err_t library_start(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    media_set_next(next_track);
    // The card's D3 line is on the expander; low during init would put the
    // card into SPI mode.
    tca9555_set_output(BOARD_EXIO_SD_D3, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = mount(false);
    xSemaphoreGive(s_lock);
    return err;
}
