/*
 * Local credential overrides — copy to secrets.h (which is git-ignored)
 * and uncomment the lines you want to bake into the build.
 *
 * Precedence on each wake:
 *   1. Values in NVS (set via the captive portal)  -- always win if present
 *   2. Values defined here                          -- fallback for empty NVS
 *   3. Otherwise                                    -- captive portal triggers
 *
 * Use these to bypass the portal during development -- no NVS, no phone-tap
 * dance every time you flash a fresh board. Leaving any of them undefined is
 * fine; the portal will collect whatever's missing.
 *
 * IMPORTANT: secrets.h itself must NEVER be committed. .gitignore is wired
 * to ignore it; double-check before pushing.
 */
#pragma once

/* ---- WiFi ---------------------------------------------------------- */
// #define WIFI_DEFAULT_SSID  "your-network"
// #define WIFI_DEFAULT_PASS  "your-password"     /* "" for open networks */

/* ---- MQTT ---------------------------------------------------------- */
// #define MQTT_DEFAULT_URI          "mqtt://192.168.1.50:1883"   /* mqtts:// for TLS */
// #define MQTT_DEFAULT_USER         "broker-user"     /* leave undefined if open */
// #define MQTT_DEFAULT_PASS         "broker-pass"
//
// Topic namespace. Defaults are tesserae/esp32/{frame/bin,config,status} --
// override only if you're avoiding a collision with another tenant on a
// shared broker, or running multiple Tesserae devices off one server.
// #define MQTT_DEFAULT_TOPIC        "tesserae/esp32/frame/bin" /* frame URLs we listen for */
// #define MQTT_DEFAULT_CONFIG_TOPIC "tesserae/esp32/config"    /* runtime settings we listen for */
// #define MQTT_DEFAULT_STATUS_TOPIC "tesserae/esp32/status"    /* heartbeats we publish */
//
// #define MQTT_CLIENT_ID            "tesserae-esp32-bin-1"     /* unique per device on the broker */

/* ---- Dev mode ------------------------------------------------------ */
/* The firmware auto-detects a connected USB host (laptop) and skips deep
 * sleep in that case, so you usually DON'T need DEV_DISABLE_SLEEP just
 * for development. Define it only when you want to force the loop without
 * a USB host (e.g. wall-mounted install on a USB charger -- the charger
 * doesn't emit SOF packets so auto-detection treats it as battery). */
// #define DEV_DISABLE_SLEEP
// #define DEV_LOOP_INTERVAL_S 10                /* defaults to 10 */
