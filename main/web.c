#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "library.h"
#include "ota.h"
#include "settings.h"
#include "voice.h"
#include "wakeword.h"
#include "wwmodel.h"
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>esp32-speaker</title>"
"<style>body{font:15px system-ui,sans-serif;max-width:32em;margin:1em auto;padding:0 1em}"
"input,select,button{font:inherit;width:100%;margin:.3em 0;padding:.4em;box-sizing:border-box}"
"pre{background:#eee;padding:.6em;white-space:pre-wrap}</style></head><body>"
"<h2>esp32-speaker</h2><p><a href=/music>Music library</a></p><pre id=st>...</pre>"
"<h3>Wi-Fi</h3><form method=post action=/wifi>"
"<select id=nets onchange=\"ssid.value=this.value\"><option>scanning...</option></select>"
"<input id=ssid name=ssid placeholder=SSID required><input name=pass type=password placeholder=Password>"
"<button>Save and connect</button></form>"
"<script>"
"fetch('/status').then(r=>r.json()).then(j=>st.textContent=JSON.stringify(j,null,1));"
"fetch('/wifi/scan').then(r=>r.json()).then(a=>{nets.innerHTML='<option value=\"\">choose a network</option>'+"
"a.map(n=>`<option>${n.ssid.replace(/</g,'&lt;')}</option>`).join('')});"
"</script></body></html>";

bool web_authorized(httpd_req_t *req)
{
    char want[65], got[65] = "";
    if (settings_get_str("token", want, sizeof(want)) != ESP_OK || !want[0]) return true;
    if (httpd_req_get_hdr_value_str(req, "X-Token", got, sizeof(got)) == ESP_OK && !strcmp(got, want)) return true;
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "token required");
    return false;
}

int web_recv(httpd_req_t *req, char *buf, size_t len)
{
    // httpd_req_recv() times out every recv_wait_timeout (5 s); retrying
    // forever let a vanished client hold the only server worker, and the whole
    // web interface hung. Three in a row (~15 s of silence) end the request.
    for (int tries = 0; tries < 3; tries++) {
        int n = httpd_req_recv(req, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
    }
    ESP_LOGW(TAG, "%s: client stalled, dropping the request", req->uri);
    return HTTPD_SOCK_ERR_TIMEOUT;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    char wifi[160], buf[512];
    wifi_status(wifi, sizeof(wifi));
    for (char *p = wifi; *p; p++) if (*p == '"') *p = '\'';
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_get_state_partition(run, &st);
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"host\":\"%s\",\"uptime_s\":%lld,\"wifi\":\"%s\",\"rssi\":%d,"
             "\"heap_internal\":%u,\"heap_psram\":%u,\"partition\":\"%s\",\"confirmed\":%s}",
             esp_app_get_description()->version, wifi_hostname(), esp_timer_get_time() / 1000000,
             wifi, wifi_rssi(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), run->label,
             st == ESP_OTA_IMG_PENDING_VERIFY ? "false" : "true");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char *buf = malloc(3072);
    if (!buf) return httpd_resp_send_500(req);
    int n = wifi_scan_json(buf, 3072);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

// Decodes application/x-www-form-urlencoded value `key` from `body`.
static bool form_get(const char *body, const char *key, char *out, size_t len)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        size_t n = 0;
        for (const char *v = p + kl + 1; *v && *v != '&' && n + 1 < len; v++) {
            if (*v == '+') out[n++] = ' ';
            else if (*v == '%' && v[1] && v[2]) { char h[3] = {v[1], v[2], 0}; out[n++] = strtol(h, NULL, 16); v += 2; }
            else out[n++] = *v;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char body[256];
    int n = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int got = httpd_req_recv(req, body, n);
    if (got <= 0) return httpd_resp_send_500(req);
    body[got] = 0;
    char ssid[33], pass[65] = "";
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    form_get(body, "pass", pass, sizeof(pass));
    httpd_resp_sendstr(req, "Saved. Joining the network; this access point closes once connected. "
                            "Then open http://esp32-speaker-XXXXXX.local (see the serial log).");
    wifi_save_and_connect(ssid, pass);
    return ESP_OK;
}

static void reboot_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    ota_job_t *job;
    esp_err_t err = ota_job_begin(&job);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    char *buf = malloc(4096);
    int left = req->content_len;
    while (buf && left > 0) {
        int n = web_recv(req, buf, left < 4096 ? left : 4096);
        if (n <= 0 || (err = ota_job_write(job, buf, n)) != ESP_OK) break;
        left -= n;
    }
    free(buf);
    if (left > 0 || err != ESP_OK) {
        ota_job_abort(job);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    if ((err = ota_job_finish(job)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    httpd_resp_sendstr(req, "OK, rebooting\n");
    xTaskCreate(reboot_later, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /wwtest: raw 16 kHz mono pcm16 fed straight to the wake word detector
// (bypassing the mic), paced at real time. Returns the peak probability.
static esp_err_t h_wwtest(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    int left = req->content_len;
    if (left <= 0 || left > 16000 * 2 * 15 || (left & 1))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "1..15 s of pcm16 (even byte count)");
    int16_t buf[320];
    uint8_t *bytes = (uint8_t *)buf;
    int have = 0;   // bytes in buf, possibly ending mid-sample
    wakeword_test_begin();
    while (left > 0) {
        int n = web_recv(req, (char *)bytes + have, left < (int)sizeof(buf) - have ? left : (int)sizeof(buf) - have);
        if (n <= 0) break;
        left -= n;
        have += n;
        int samples = have / 2;
        wakeword_feed_test(buf, samples);
        vTaskDelay(pdMS_TO_TICKS(samples / 16));   // pace at real time (16 samples/ms)
        if (have & 1) bytes[0] = bytes[have - 1];  // carry a split sample over
        have &= 1;
    }
    float peak;
    int hits;
    wakeword_test_end(&peak, &hits);
    char out[96];
    snprintf(out, sizeof(out), "{\"peak\":%.3f,\"detections\":%d,\"infer_us\":%lu}", peak, hits,
             (unsigned long)wakeword_last_us());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// POST /ask: raw 16 kHz mono pcm16 sent to the voice server as an utterance
// (as if spoken into the mic); the reply plays on the speaker as usual.
static esp_err_t h_ask(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 16000 * 2 * 15) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "1..15 s of pcm16");
    int16_t *pcm = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!pcm) return httpd_resp_send_500(req);
    int got = 0;
    while (got < len) {
        int n = web_recv(req, (char *)pcm + got, len - got);
        if (n <= 0) break;
        got += n;
    }
    esp_err_t err = got == len ? voice_ask_injected(pcm, len / 2) : ESP_FAIL;
    free(pcm);
    return err == ESP_OK ? httpd_resp_sendstr(req, "sent; the reply plays on the speaker\n")
                         : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
}

static float query_float(httpd_req_t *req, const char *key, float def)
{
    char q[160], v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK)
        return def;
    return strtof(v, NULL);
}

// POST /wwmodel?cutoff=0.9&window=5&arena=40000&name=x with the .tflite as the
// body: stores the wake word model in flash and reboots. DELETE reverts to the
// built-in model.
static esp_err_t h_wwmodel(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    char q[160], name[32] = "uploaded";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "name", name, sizeof(name));
    wwmodel_writer_t *w;
    esp_err_t err = wwmodel_write_begin(&w, req->content_len, query_float(req, "cutoff", 0.9f),
                                        (int)query_float(req, "window", 5), (int)query_float(req, "arena", 40000), name);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    char *buf = malloc(4096);
    int left = req->content_len;
    while (buf && left > 0) {
        int n = web_recv(req, buf, left < 4096 ? left : 4096);
        if (n <= 0 || (err = wwmodel_write(w, buf, n)) != ESP_OK) break;
        left -= n;
    }
    free(buf);
    if (left > 0 || err != ESP_OK) {
        wwmodel_write_abort(w);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "model upload failed");
    }
    if ((err = wwmodel_write_finish(w)) != ESP_OK)   // validates the model; frees w either way
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a usable wake word model");
    httpd_resp_sendstr(req, "OK, rebooting\n");
    xTaskCreate(reboot_later, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_wwmodel_delete(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    wwmodel_erase();
    httpd_resp_sendstr(req, "OK, rebooting\n");
    xTaskCreate(reboot_later, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!web_authorized(req)) return ESP_OK;
    httpd_resp_sendstr(req, "rebooting\n");
    xTaskCreate(reboot_later, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Captive portal: send every unknown URL to the setup page while the AP is up.
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t code)
{
    if (!wifi_provisioning()) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// Minimal DNS responder for the provisioning AP: answers every A query with the
// AP address so phones pop up the captive portal.
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        vTaskDelete(NULL);
    }
    uint8_t q[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !wifi_provisioning()) continue;
        q[2] = 0x81; q[3] = 0x80;                     // response, recursion available
        q[6] = 0; q[7] = 1;                           // one answer
        q[8] = q[9] = q[10] = q[11] = 0;
        static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(q + n, ans, sizeof(ans));
        sendto(s, q, n + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 24;   // default is 8; registering past it fails silently
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/wifi/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/wwtest", .method = HTTP_POST, .handler = h_wwtest},
        {.uri = "/ask", .method = HTTP_POST, .handler = h_ask},
        {.uri = "/wwmodel", .method = HTTP_POST, .handler = h_wwmodel},
        {.uri = "/wwmodel", .method = HTTP_DELETE, .handler = h_wwmodel_delete},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(srv, &uris[i]);
    library_register(srv);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_404);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    return ESP_OK;
}
