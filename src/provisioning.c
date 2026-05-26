#include "provisioning.h"
#include "app_config.h"
#include "mqtt_config.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

#define BIT_CREDS_SAVED  BIT0
static EventGroupHandle_t s_done;
static TaskHandle_t s_dns_task = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;

/* ---------- minimal wildcard DNS hijack ----------
 *
 * Listens on UDP/53 and answers every A query with our AP IP (192.168.4.1).
 * Phones probe known URLs (captive.apple.com, connectivitycheck.gstatic.com,
 * etc.); routing those to us makes the OS pop up our HTTP form automatically.
 *
 * We don't parse the DNS query body -- we just copy the question section
 * back, set QR=1 / AA=1 / RCODE=0, and append one A-record answer.        */

static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "dns sock fail"); vTaskDelete(NULL); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind fail");
        close(sock); vTaskDelete(NULL);
    }

    /* 192.168.4.1 in network byte order */
    const uint8_t our_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) continue;   /* DNS header is 12 bytes minimum */

        /* Flip QR (response), AA (authoritative), set ANCOUNT=1 */
        buf[2] = 0x84;   /* QR=1 AA=1 OPCODE=0 */
        buf[3] = 0x00;   /* RCODE=0 */
        buf[6] = 0x00; buf[7] = 0x01;   /* ANCOUNT */
        buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT */
        buf[10]= 0x00; buf[11]= 0x00;   /* ARCOUNT */

        /* Append answer: pointer to qname (0xC00C), TYPE A, CLASS IN,
         * TTL=60, RDLENGTH=4, RDATA=192.168.4.1 */
        int p = n;
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        buf[p++] = 0x00; buf[p++] = 0x01;   /* TYPE A */
        buf[p++] = 0x00; buf[p++] = 0x01;   /* CLASS IN */
        buf[p++] = 0x00; buf[p++] = 0x00;   /* TTL hi */
        buf[p++] = 0x00; buf[p++] = 0x3C;   /* TTL = 60s */
        buf[p++] = 0x00; buf[p++] = 0x04;   /* RDLENGTH */
        for (int i = 0; i < 4; i++) buf[p++] = our_ip[i];

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }
}

/* ---------- HTTP form ---------- */

static const char k_form_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Tesserae Setup</title>"
"<style>"
"body{font-family:system-ui,sans-serif;max-width:420px;margin:1.5em auto;padding:0 1em;color:#222}"
"h1{font-size:1.3em}h2{font-size:1.05em;margin:1.8em 0 .2em;color:#555}"
"label{display:block;margin:.9em 0 .3em;font-size:.95em}"
"input{width:100%;padding:.6em;font-size:1em;box-sizing:border-box}"
"small{display:block;color:#888;margin-top:.25em;font-size:.8em}"
"button{margin-top:1.5em;padding:.7em 1.2em;font-size:1em;width:100%}"
"</style></head><body>"
"<h1>Tesserae Setup</h1>"
"<form method=\"POST\" action=\"/save\">"

"<h2>WiFi</h2>"
"<label>Network name (SSID) *</label>"
"<input name=\"ssid\" required maxlength=\"32\" autocomplete=\"off\">"
"<label>Password</label>"
"<input name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"

"<h2>MQTT broker</h2>"
"<label>Broker URI *</label>"
"<input name=\"mqtt_uri\" required maxlength=\"159\" autocomplete=\"off\" "
"placeholder=\"mqtt://192.168.1.50:1883\">"
"<small>Use <code>mqtts://</code> for TLS.</small>"
"<label>Topic</label>"
"<input name=\"mqtt_topic\" maxlength=\"95\" autocomplete=\"off\" "
"placeholder=\"" MQTT_DEFAULT_TOPIC "\">"
"<label>Username (optional)</label>"
"<input name=\"mqtt_user\" maxlength=\"63\" autocomplete=\"off\">"
"<label>Password (optional)</label>"
"<input name=\"mqtt_pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"

"<button type=\"submit\">Save &amp; restart</button>"
"</form></body></html>";

static const char k_thanks_html[] =
"<!doctype html><html><body style=\"font-family:system-ui;text-align:center;margin:3em\">"
"<h2>Saved.</h2><p>The dash will reboot and try to join now.</p>"
"</body></html>";

/* URL-decode %xx and '+' in-place. Caller-owned buffer. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

/* Pull a named field out of x-www-form-urlencoded body into dst. */
static bool form_field(const char *body, const char *key, char *dst, size_t dst_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= dst_sz) len = dst_sz - 1;
            memcpy(dst, v, len);
            dst[len] = '\0';
            url_decode(dst);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, k_form_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_save(httpd_req_t *req)
{
    /* Form is now ~6 fields with URI + topic + creds; budget room for the
     * longest legitimate POST body (~750 bytes after URL-encoding). */
    char body[1024];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';

    char ssid[33] = {0}, wpa_pass[65] = {0};
    char mqtt_uri[160] = {0}, mqtt_topic[96] = {0};
    char mqtt_user[64] = {0}, mqtt_pass[64] = {0};

    if (!form_field(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    if (!form_field(body, "mqtt_uri", mqtt_uri, sizeof(mqtt_uri)) || !mqtt_uri[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mqtt broker uri required");
        return ESP_FAIL;
    }
    form_field(body, "pass",       wpa_pass,   sizeof(wpa_pass));
    form_field(body, "mqtt_topic", mqtt_topic, sizeof(mqtt_topic));
    form_field(body, "mqtt_user",  mqtt_user,  sizeof(mqtt_user));
    form_field(body, "mqtt_pass",  mqtt_pass,  sizeof(mqtt_pass));

    ESP_LOGI(TAG, "saving wifi='%s' mqtt='%s' topic='%s'",
             ssid, mqtt_uri, mqtt_topic[0] ? mqtt_topic : "(default)");

    if (wifi_creds_save(ssid, wpa_pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi nvs write failed");
        return ESP_FAIL;
    }
    if (mqtt_config_save(mqtt_uri, mqtt_topic, mqtt_user, mqtt_pass) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mqtt nvs write failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, k_thanks_html, HTTPD_RESP_USE_STRLEN);
    xEventGroupSetBits(s_done, BIT_CREDS_SAVED);
    return ESP_OK;
}

/* Catch-all: redirect anything else to "/" so captive-portal probes land
 * on the form regardless of which URL the OS picked. */
static esp_err_t h_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------- portal lifecycle ---------- */

static void start_ap(void)
{
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wc.ap.ssid,     PROVISION_AP_SSID, sizeof(wc.ap.ssid) - 1);
    strncpy((char *)wc.ap.password, PROVISION_AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.ssid_len = strlen(PROVISION_AP_SSID);
    if (strlen(PROVISION_AP_PASS) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP up: ssid=%s ip=192.168.4.1", PROVISION_AP_SSID);
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 6;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET,  .handler = h_catchall };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &any);
}

esp_err_t provisioning_run_blocking(void)
{
    s_done = xEventGroupCreate();

    start_ap();
    start_http();
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "portal up; waiting up to %ds for submission",
             PROVISION_PORTAL_TIMEOUT_S);
    EventBits_t bits = xEventGroupWaitBits(
        s_done, BIT_CREDS_SAVED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(PROVISION_PORTAL_TIMEOUT_S * 1000));

    /* Give the browser a beat to render the "saved" page before we tear AP down. */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    if (s_httpd)    { httpd_stop(s_httpd);     s_httpd = NULL; }
    esp_wifi_stop();

    return (bits & BIT_CREDS_SAVED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
