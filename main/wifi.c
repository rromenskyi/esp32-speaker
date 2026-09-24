#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "settings.h"

static const char *TAG = "wifi";

#define JOIN_TIMEOUT_US   (30 * 1000000LL)   // open the portal if not joined by then
#define AP_LINGER_MS      8000               // keep the portal up briefly after joining

static char s_host[32];
static char s_ssid[33];
static esp_netif_t *s_sta, *s_ap;
static volatile bool s_connected, s_ap_on;
static int s_retries;
static esp_timer_handle_t s_join_timer, s_ap_off_timer, s_retry_timer;

static void ap_enable(bool on)
{
    if (on == s_ap_on) return;
    s_ap_on = on;
    ESP_ERROR_CHECK(esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_LOGI(TAG, "provisioning AP \"%s\" %s", s_host, on ? "up (http://192.168.4.1)" : "down");
}

static void join_timeout_cb(void *arg)
{
    if (!s_connected) ap_enable(true);
}

static void ap_off_cb(void *arg)
{
    if (s_connected) ap_enable(false);
}

static void retry_cb(void *arg)
{
    if (s_ssid[0] && !s_connected) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_connected) ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        s_connected = false;
        if (!s_ssid[0]) return;
        // Back off up to ~30 s; the portal (if open) keeps working meanwhile.
        int delay_ms = s_retries < 5 ? 1000 : s_retries < 20 ? 5000 : 30000;
        s_retries++;
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, delay_ms * 1000LL);   // never block the event loop
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected to \"%s\", ip " IPSTR ", http://%s.local", s_ssid, IP2STR(&e->ip_info.ip), s_host);
        s_connected = true;
        s_retries = 0;
        esp_timer_stop(s_join_timer);
        if (s_ap_on) esp_timer_start_once(s_ap_off_timer, AP_LINGER_MS * 1000LL);
    }
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(s_host);
    mdns_instance_name_set("esp32-speaker");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t wifi_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_host, sizeof(s_host), "esp32-speaker-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s_host);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // credentials live in settings
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t ap = {.ap = {.max_connection = 2, .authmode = WIFI_AUTH_OPEN, .channel = 1}};
    strlcpy((char *)ap.ap.ssid, s_host, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_host);

    wifi_config_t sta = {0};
    char pass[65] = "";
    if (settings_get_str("wifi_ssid", s_ssid, sizeof(s_ssid)) == ESP_OK) {
        settings_get_str("wifi_pass", pass, sizeof(pass));
        strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    } else {
        s_ssid[0] = 0;
    }

    esp_timer_create_args_t t1 = {.callback = join_timeout_cb, .name = "wifi_join"};
    esp_timer_create_args_t t2 = {.callback = ap_off_cb, .name = "wifi_ap_off"};
    ESP_ERROR_CHECK(esp_timer_create(&t1, &s_join_timer));
    esp_timer_create_args_t t3 = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&t2, &s_ap_off_timer));
    ESP_ERROR_CHECK(esp_timer_create(&t3, &s_retry_timer));

    s_ap_on = !s_ssid[0];
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Default modem power save drops a large share of packets on IDF v6; this is a
    // mains-powered device that streams audio, so keep the radio awake.
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (s_ap_on) ESP_LOGI(TAG, "no saved network, provisioning AP \"%s\" up (http://192.168.4.1)", s_host);
    else esp_timer_start_once(s_join_timer, JOIN_TIMEOUT_US);
    mdns_start();
    return ESP_OK;
}

esp_err_t wifi_save_and_connect(const char *ssid, const char *pass)
{
    esp_err_t err = settings_set_str("wifi_ssid", ssid);
    if (err == ESP_OK) err = settings_set_str("wifi_pass", pass ? pass : "");
    if (err != ESP_OK) return err;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password));
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    s_retries = 0;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    return esp_wifi_connect();
}

bool wifi_connected(void) { return s_connected; }
bool wifi_provisioning(void) { return s_ap_on; }
const char *wifi_hostname(void) { return s_host; }

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void wifi_status(char *out, size_t len)
{
    esp_netif_ip_info_t ip = {0};
    if (s_connected) esp_netif_get_ip_info(s_sta, &ip);
    snprintf(out, len, "%s ssid=\"%s\" ip=" IPSTR " rssi=%d portal=%s host=%s.local",
             s_connected ? "connected" : "disconnected", s_ssid, IP2STR(&ip.ip), wifi_rssi(),
             s_ap_on ? "on" : "off", s_host);
}

static size_t json_str(char *out, size_t len, const char *s)
{
    size_t n = 0;
    if (n < len) out[n++] = '"';
    for (; *s && n + 7 < len; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c < 0x20) n += snprintf(out + n, len - n, "\\u%04x", c);
        else out[n++] = c;
    }
    if (n < len) out[n++] = '"';
    return n;
}

int wifi_scan_json(char *out, size_t len)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return snprintf(out, len, "[]");
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t p = snprintf(out, len, "[");
    for (int i = 0; i < n && p + 80 < len; i++) {
        if (!recs[i].ssid[0]) continue;
        if (p > 1) out[p++] = ',';
        p += snprintf(out + p, len - p, "{\"ssid\":");
        p += json_str(out + p, len - p, (const char *)recs[i].ssid);
        p += snprintf(out + p, len - p, ",\"rssi\":%d,\"auth\":%d}", recs[i].rssi, recs[i].authmode);
    }
    p += snprintf(out + p, len - p, "]");
    return p;
}
