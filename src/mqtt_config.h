/*
 * NVS-backed MQTT settings.
 *
 * Provisioning (the captive portal) writes these on first boot, mqtt_handler
 * reads them on every wake. Any field missing from NVS falls back to the
 * compile-time defaults in app_config.h.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char uri[160];        /* mqtt://host:port or mqtts://host:port */
    char topic[96];
    char user[64];        /* empty if broker is open */
    char pass[64];        /* empty if broker is open */
} mqtt_config_t;

/* Fill `out` from NVS, with defaults from app_config.h for any missing key.
 * Never fails -- worst case is all defaults. */
void mqtt_config_load(mqtt_config_t *out);

/* Persist all four fields. Empty strings are stored as empty (caller's choice
 * to clear a field by passing ""). */
esp_err_t mqtt_config_save(const char *uri, const char *topic,
                           const char *user, const char *pass);
