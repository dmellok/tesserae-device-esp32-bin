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
