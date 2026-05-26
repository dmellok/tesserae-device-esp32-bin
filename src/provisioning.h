/*
 * SoftAP captive-portal WiFi provisioning.
 *
 * Brings up an AP, a wildcard DNS responder so phones auto-trigger their
 * "sign in to network" prompt, and an HTTP form. Blocks until the user
 * submits creds (which are then persisted via wifi_creds_save()) or the
 * timeout fires.
 */
#pragma once

#include "esp_err.h"

/* Runs the portal in the calling task. Returns ESP_OK if creds were saved,
 * ESP_ERR_TIMEOUT if the portal timed out with no submission. */
esp_err_t provisioning_run_blocking(void);
