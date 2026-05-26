#include "mqtt_config.h"
#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "mqtt_cfg";

static void load_str(nvs_handle_t h, const char *key,
                     char *dst, size_t dst_sz, const char *fallback)
{
    size_t len = dst_sz;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        strncpy(dst, fallback ? fallback : "", dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

/* esp-mqtt's uri parser rejects bare host:port; users typing the broker into
 * the captive portal routinely leave the scheme off. Prepend mqtt:// in-place
 * if none of the recognized schemes is present. */
static void normalize_mqtt_uri(char *uri, size_t uri_sz)
{
    if (!uri || !uri[0]) return;
    if (strncmp(uri, "mqtt://",  7) == 0) return;
    if (strncmp(uri, "mqtts://", 8) == 0) return;
    if (strncmp(uri, "ws://",    5) == 0) return;
    if (strncmp(uri, "wss://",   6) == 0) return;

    const char prefix[] = "mqtt://";
    size_t plen = sizeof(prefix) - 1;
    size_t ulen = strlen(uri);
    if (ulen + plen + 1 > uri_sz) return;   /* no room; let esp-mqtt log the error */
    memmove(uri + plen, uri, ulen + 1);
    memcpy(uri, prefix, plen);
}

void mqtt_config_load(mqtt_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQTT, NVS_READONLY, &h) != ESP_OK) {
        /* Nothing stored yet -- use compile-time defaults across the board. */
        strncpy(out->uri,   MQTT_DEFAULT_URI,   sizeof(out->uri)   - 1);
        strncpy(out->topic, MQTT_DEFAULT_TOPIC, sizeof(out->topic) - 1);
        strncpy(out->user,  MQTT_DEFAULT_USER,  sizeof(out->user)  - 1);
        strncpy(out->pass,  MQTT_DEFAULT_PASS,  sizeof(out->pass)  - 1);
        return;
    }

    load_str(h, NVS_KEY_MQTT_URI,   out->uri,   sizeof(out->uri),   MQTT_DEFAULT_URI);
    load_str(h, NVS_KEY_MQTT_TOPIC, out->topic, sizeof(out->topic), MQTT_DEFAULT_TOPIC);
    load_str(h, NVS_KEY_MQTT_USER,  out->user,  sizeof(out->user),  MQTT_DEFAULT_USER);
    load_str(h, NVS_KEY_MQTT_PASS,  out->pass,  sizeof(out->pass),  MQTT_DEFAULT_PASS);

    nvs_close(h);

    normalize_mqtt_uri(out->uri, sizeof(out->uri));

    ESP_LOGI(TAG, "loaded uri='%s' topic='%s' user='%s'",
             out->uri, out->topic, out->user[0] ? out->user : "(none)");
}

esp_err_t mqtt_config_save(const char *uri, const char *topic,
                           const char *user, const char *pass)
{
    if (!uri || !*uri) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_MQTT_URI, uri);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_MQTT_TOPIC,
                          (topic && *topic) ? topic : MQTT_DEFAULT_TOPIC);
    }
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_USER, user ? user : "");
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_MQTT_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
