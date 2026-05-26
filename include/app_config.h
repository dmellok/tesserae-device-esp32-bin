/*
 * Project-wide tunables. Edit here, not scattered across .c files.
 *
 * For local credential overrides (dev shortcut to bypass the captive
 * portal), copy include/secrets.example.h to include/secrets.h and fill
 * in the WIFI_DEFAULT_* / MQTT_DEFAULT_* macros there. secrets.h is
 * git-ignored.
 */
#pragma once

#include <stdint.h>

/* Pull in user-local overrides if they exist. Falls through silently if
 * secrets.h hasn't been created -- the build doesn't depend on it. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* ------------------------------------------------------------------ */
/* Panel: Waveshare ESP32-S3-ePaper-13.3E6                            */
/* Pinout copied verbatim from the official ESP-IDF demo:             */
/*   waveshareteam/ESP32-S3-ePaper-13.3E6 (epaper_port.h)             */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   9
#define EPD_PIN_MOSI   46
#define EPD_PIN_CS_M   10   /* drives the left  half (cols   0..599)  */
#define EPD_PIN_CS_S   3    /* drives the right half (cols 600..1199) */
#define EPD_PIN_DC     11
#define EPD_PIN_RST    2
#define EPD_PIN_BUSY   12
#define EPD_PIN_PWR    1    /* active-high panel power enable          */

#define EPD_SPI_HOST   SPI3_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)

/* Panel geometry. Native orientation is portrait. */
#define EPD_WIDTH      1200
#define EPD_HEIGHT     1600
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 960000 */

/* 6-color palette indices (nibble values). 4 and 7 are reserved. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* ------------------------------------------------------------------ */
/* Application behavior                                               */
/* ------------------------------------------------------------------ */

/* Reported in the status heartbeat so Tesserae can show which firmware
 * each device is running. Bump on every release. */
#define FW_VERSION         "0.1.0"

/* How long to deep-sleep between MQTT checks. 15 minutes is a good
 * trade for a 6-color panel that itself takes ~30s to refresh. */
#define SLEEP_INTERVAL_S   (15 * 60)

/* Cap on how long we'll wait for a retained MQTT message after
 * subscribing, before giving up and going back to sleep. */
#define MQTT_WAIT_MS       8000

/* WiFi STA connect attempt: retry count and per-attempt timeout. */
#define WIFI_CONNECT_RETRIES   5
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* How long the provisioning portal stays up after a failed STA
 * connect. Power gets burned the whole time, so don't make it huge. */
#define PROVISION_PORTAL_TIMEOUT_S  (10 * 60)

/* SoftAP credentials shown to the user during provisioning. */
#define PROVISION_AP_SSID    "Tesserae-Setup"
#define PROVISION_AP_PASS    "tesserae"     /* >= 8 chars or use open AP */

/* ------------------------------------------------------------------ */
/* WiFi / MQTT compile-time defaults                                  */
/* ------------------------------------------------------------------ */
/* Precedence on each wake:
 *     NVS (set via portal)  >  these defaults  >  empty (portal triggers)
 *
 * secrets.h may override any of these; otherwise WiFi defaults to empty
 * (no auto-connect) and MQTT defaults to placeholders that will fail
 * gracefully if the user hasn't run the portal yet. */

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID   ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS   ""
#endif

#ifndef MQTT_DEFAULT_URI
#define MQTT_DEFAULT_URI    "mqtt://homeassistant.local:1883"
#endif
#ifndef MQTT_DEFAULT_TOPIC
#define MQTT_DEFAULT_TOPIC  "tesserae/esp32/frame/bin"
#endif
#ifndef MQTT_DEFAULT_CONFIG_TOPIC
#define MQTT_DEFAULT_CONFIG_TOPIC "tesserae/esp32/config"
#endif
#ifndef MQTT_DEFAULT_STATUS_TOPIC
#define MQTT_DEFAULT_STATUS_TOPIC "tesserae/esp32/status"
#endif
#ifndef MQTT_DEFAULT_USER
#define MQTT_DEFAULT_USER   ""
#endif
#ifndef MQTT_DEFAULT_PASS
#define MQTT_DEFAULT_PASS   ""
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID      "tesserae-esp32-bin-1"
#endif

/* Dev shortcut: define DEV_DISABLE_SLEEP (in secrets.h) to swap the
 * 15-min deep sleep for a short delay + software restart loop. Useful
 * while iterating with the serial monitor open. Cold-boot splash only
 * fires on power-on / RESET button, not on the software restart, so
 * each iteration is fast. */
#ifndef DEV_LOOP_INTERVAL_S
#define DEV_LOOP_INTERVAL_S 10
#endif

/* NVS namespaces / keys */
#define NVS_NS_WIFI        "wifi"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"

#define NVS_NS_MQTT        "mqtt"
#define NVS_KEY_MQTT_URI   "uri"
#define NVS_KEY_MQTT_TOPIC "topic"
#define NVS_KEY_MQTT_USER  "user"
#define NVS_KEY_MQTT_PASS  "pass"

#define NVS_NS_STATE       "state"
#define NVS_KEY_LAST_HASH  "last_hash"   /* sha256 of last rendered URL */
#define NVS_KEY_SLEEP_S    "sleep_s"     /* user-configured deep-sleep duration */

/* Sanity bounds on sleep interval. The lower bound stops a publisher
 * accidentally turning the device into a 1-Hz spinner; the upper bound
 * is just "this is probably a bug". */
#define SLEEP_INTERVAL_MIN_S  30
#define SLEEP_INTERVAL_MAX_S  (7 * 24 * 60 * 60)
