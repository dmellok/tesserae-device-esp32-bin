/*
 * tesserae-esp32-bin-client - battery-powered MQTT-driven e-paper client
 * for the Tesserae composer/renderer pipeline. Subscribes to a retained
 * frame URL, downloads the panel-native 4bpp .bin, paints it, and goes
 * back to deep sleep.
 *
 * Lifecycle of one wake:
 *
 *   boot
 *     -> wifi creds in NVS?           no  -> captive portal -> reboot
 *     -> connect STA                  fail-> captive portal -> reboot
 *     -> grab retained MQTT job       miss-> sleep (nothing new to show)
 *     -> url unchanged since last?    yes -> sleep (skip refresh)
 *     -> fetch + decode + paint panel fail-> sleep (try again next wake)
 *     -> persist new hash             -> deep sleep for SLEEP_INTERVAL_S
 */

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "app_config.h"
#include "epd_driver.h"
#include "heartbeat.h"
#include "image_decoder.h"
#include "image_fetcher.h"
#include "mqtt_handler.h"
#include "provisioning.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* ---------- double-tap-reset -> settings mode ---------- */

/* The tap counter lives in RTC slow memory so it survives the RESET button
 * (and deep sleep). A magic word tells "retained" apart from power-on garbage.
 *
 * NOTE: whether the RESET button preserves RTC memory is board-specific. If a
 * given board fully power-cycles the RTC domain on reset, the counter never
 * reaches 2 and double-tap simply never fires -- a harmless no-op. In that
 * case use the captive portal (no creds) or `idf.py erase-flash` instead. */
#define RTC_TAP_MAGIC  0x54455353u   /* 'TESS' */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_reset_taps;

/* Increment on each manual reset; two within one wake window => settings mode.
 * The window is closed by zeroing the counter when we commit to deep sleep
 * (see sleep_forever_or_until_timer), so single taps minutes apart don't add
 * up to a false double-tap. */
static bool detect_settings_mode(esp_reset_reason_t reason)
{
    if (s_rtc_magic != RTC_TAP_MAGIC) {   /* power-on / garbage: seed it */
        s_rtc_magic = RTC_TAP_MAGIC;
        s_reset_taps = 0;
    }

    bool manual = (reason == ESP_RST_POWERON || reason == ESP_RST_EXT);
    if (manual) {
        s_reset_taps++;
    } else {
        s_reset_taps = 0;   /* timer wake / software restart isn't a tap */
    }

    if (s_reset_taps >= 2) {
        s_reset_taps = 0;
        return true;
    }
    return false;
}

/* ---------- "should I bother re-rendering?" ---------- */

static void sha256_hex(const char *in, char out_hex[65])
{
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)in, strlen(in), digest, 0);
    for (int i = 0; i < 32; i++) {
        static const char H[] = "0123456789abcdef";
        out_hex[i*2]   = H[(digest[i] >> 4) & 0xF];
        out_hex[i*2+1] = H[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

static bool hash_matches_stored(const char *hash)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) != ESP_OK) return false;
    char prev[65] = {0};
    size_t len = sizeof(prev);
    esp_err_t err = nvs_get_str(h, NVS_KEY_LAST_HASH, prev, &len);
    nvs_close(h);
    return err == ESP_OK && strcmp(prev, hash) == 0;
}

static void store_hash(const char *hash)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_LAST_HASH, hash);
    nvs_commit(h);
    nvs_close(h);
}

/* Read the MQTT-configured sleep interval from NVS, falling back to the
 * compile-time SLEEP_INTERVAL_S default. Clamped defensively in case
 * something wrote a bad value before the bounds check existed. */
static int load_sleep_interval_s(void)
{
    int32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_i32(h, NVS_KEY_SLEEP_S, &v);
        nvs_close(h);
        if (err == ESP_OK &&
            v >= SLEEP_INTERVAL_MIN_S &&
            v <= SLEEP_INTERVAL_MAX_S) {
            return v;
        }
    }
    return SLEEP_INTERVAL_S;
}

/* ---------- deep sleep ---------- */

static void sleep_forever_or_until_timer(void)
{
    /* Decide between deep sleep (battery) and short-delay restart loop
     * (dev). Either DEV_DISABLE_SLEEP forces the loop, OR a connected
     * USB host (laptop / SOF-emitting host) auto-selects it -- a bare
     * USB charger / power bank doesn't emit SOFs and is treated as
     * battery operation. */
    /* Close the double-tap window: once we're committing to sleep/loop, a
     * later single reset should start counting from zero again. */
    s_reset_taps = 0;

    bool loop = false;
    const char *reason = NULL;

#ifdef DEV_DISABLE_SLEEP
    loop = true;
    reason = "DEV_DISABLE_SLEEP";
#else
    if (usb_serial_jtag_is_connected()) {
        loop = true;
        reason = "USB host detected";
    }
#endif

    if (loop) {
        ESP_LOGI(TAG, "%s: software restart in %d s", reason, DEV_LOOP_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(DEV_LOOP_INTERVAL_S * 1000));
        esp_restart();
    }

    int interval = load_sleep_interval_s();
    ESP_LOGI(TAG, "on battery; deep sleep for %d s%s",
             interval,
             (interval == SLEEP_INTERVAL_S) ? " (default)" : " (set via mqtt)");
    /* epd_sleep() already dropped the panel power rail; no extra cleanup
     * needed before going down. */
    esp_sleep_enable_timer_wakeup((uint64_t)interval * 1000000ULL);
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- app ---------- */

static void run_provisioning_then_reboot(void)
{
    ESP_LOGW(TAG, "no usable wifi creds; starting captive portal");
    esp_err_t err = provisioning_run_blocking();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "creds saved; rebooting to use them");
    } else {
        ESP_LOGW(TAG, "portal timed out; sleeping and trying again later");
    }
    /* Either way: reboot so STA path starts fresh next time. */
    esp_restart();
}

/* Show the splash on a true cold boot -- power-on or RESET button. Skip
 * it on timer-wake (production sleep cycle) AND on software restart
 * (DEV_DISABLE_SLEEP loop iterations), so we don't burn 25-30 s of panel
 * refresh on every quick test cycle. */
static void maybe_show_splash(esp_reset_reason_t reset_reason)
{
    if (reset_reason != ESP_RST_POWERON && reset_reason != ESP_RST_EXT) {
        return;
    }
    ESP_LOGI(TAG, "cold boot; showing splash");
    if (epd_port_init() != ESP_OK) {
        ESP_LOGW(TAG, "panel init failed; skipping splash");
        return;
    }
    epd_init();
    epd_show_color_bars();
    epd_sleep();
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool settings_mode = detect_settings_mode(reset_reason);
    ESP_LOGI(TAG, "boot; reset_reason=%d wakeup_cause=%d settings_mode=%d",
             reset_reason, esp_sleep_get_wakeup_cause(), settings_mode);

    ESP_ERROR_CHECK(wifi_manager_init());

    /* Skip the 30 s splash when entering settings mode -- the user is waiting
     * on the editor, not a panel sanity check. */
    if (!settings_mode) {
        maybe_show_splash(reset_reason);
    }

    if (!wifi_creds_present()) {
        run_provisioning_then_reboot();
        return;
    }

    esp_err_t err = wifi_sta_connect_stored();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA connect failed (%s); falling back to portal",
                 esp_err_to_name(err));
        run_provisioning_then_reboot();
        return;
    }

    /* Double-tap reset: serve the always-on settings editor on the LAN instead
     * of running the paint cycle. Stays up until a save (then reboot) or the
     * portal timeout (then sleep). */
    if (settings_mode) {
        ESP_LOGI(TAG, "settings mode: serving LAN editor + mDNS");
        if (settings_server_run_blocking() == ESP_OK) {
            ESP_LOGI(TAG, "settings saved; rebooting to apply");
            esp_restart();
        }
        ESP_LOGI(TAG, "settings editor timed out; back to sleep");
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }

    char heartbeat[256];
    heartbeat_format_json(heartbeat, sizeof(heartbeat),
                          load_sleep_interval_s(), reset_reason);

    mqtt_job_t job;
    err = mqtt_fetch_retained(&job, heartbeat);
    if (err != ESP_OK || !job.url[0]) {
        ESP_LOGI(TAG, "no retained job (%s); back to sleep",
                 esp_err_to_name(err));
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }

    char hash[65];
    sha256_hex(job.url, hash);
    if (hash_matches_stored(hash)) {
        ESP_LOGI(TAG, "url unchanged since last render; sleeping without refresh");
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }

    fetched_image_t img;
    err = image_fetch(job.url, &img);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch failed: %s", esp_err_to_name(err));
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }
    /* Free WiFi as soon as we're done with the network -- ~80 mA savings
     * during the multi-second panel render that follows. */
    wifi_sta_stop();

    uint8_t *frame = NULL;
    err = image_decode_to_frame(&img, job.url, &frame);
    free(img.data);
    if (err != ESP_OK || !frame) {
        ESP_LOGE(TAG, "decode failed: %s", esp_err_to_name(err));
        sleep_forever_or_until_timer();
        return;
    }

    ESP_ERROR_CHECK(epd_port_init());
    epd_init();
    epd_display(frame);
    epd_sleep();
    free(frame);

    store_hash(hash);
    ESP_LOGI(TAG, "render OK; entering deep sleep");
    sleep_forever_or_until_timer();
}
