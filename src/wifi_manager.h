/*
 * WiFi credential persistence + STA connect.
 *
 * NVS layout (namespace "wifi"):
 *   ssid  : string (max 32)
 *   pass  : string (max 64, may be empty for open networks)
 *
 * The provisioning flow writes here; main.c reads + tries to connect.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialise NVS + esp-netif + esp-event. Idempotent. */
esp_err_t wifi_manager_init(void);

/* True iff non-empty SSID is stored. */
bool wifi_creds_present(void);

/* Persist creds. Empty pass is allowed (open network). */
esp_err_t wifi_creds_save(const char *ssid, const char *pass);

/* Bring up STA with stored creds and block until connected or timeout.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT / ESP_FAIL otherwise. */
esp_err_t wifi_sta_connect_stored(void);

/* Stop the STA driver; safe to call before deep sleep. */
void wifi_sta_stop(void);
