/*
 * Wake-time heartbeat: battery, signal strength, IP.
 *
 * Builds a compact JSON object suitable for publishing to a retained
 * MQTT topic so a companion dashboard can see "last known device state"
 * even while the device is asleep.
 */
#pragma once

#include <stddef.h>

/* Fill `dst` with a JSON document like:
 *   {"battery_mv":3950,"battery_pct":67,"rssi":-45,
 *    "ip":"192.168.50.234","fw_version":"0.3.0",
 *    "kind":"esp32_client","panel_w":1200,"panel_h":1600}
 *
 * Never fails -- unknown fields are emitted as 0 / empty string. The
 * caller's buffer should be at least 256 bytes; smaller is safe but
 * fields may be truncated. */
void heartbeat_format_json(char *dst, size_t dst_sz);
