# tesserae-esp32-bin-client

Battery-powered ESP32-S3 firmware for the [Waveshare ESP32-S3-ePaper-13.3E6](https://www.waveshare.com/esp32-s3-epaper-13.3e6.htm) — a 13.3", 1200×1600, 6-colour Spectra E6 e-paper panel paired with an ESP32-S3-WROOM-2-N32R16V module.

The device wakes on a timer, publishes a heartbeat with its battery state, pulls a retained MQTT message containing a `.bin` frame URL, downloads the panel-native 4-bpp buffer, paints the panel, and goes back to deep sleep. WiFi credentials and MQTT broker details are provisioned on first boot via a SoftAP captive portal.

This is the embedded counterpart to the Tesserae server's `esp32_bin` renderer — Tesserae composes a frame, packs it into the panel-native 4 bpp `.bin` format, and publishes the URL **retained** on `tesserae/esp32/frame/bin` so a battery client can connect briefly, immediately receive the most recent frame URL (no waiting), and sleep again.

## Why retained MQTT + URL hash + WiFi-before-paint

The whole architecture is in service of one constraint: **months of battery life from a single Li-Po pouch cell**. Every design choice falls out of that:

- **Retained MQTT** — no polling, no holding a connection. The client connects, the broker hands it the latest URL within hundreds of ms, the client disconnects.
- **URL hash skip** — the SHA-256 of the last rendered URL lives in NVS. If the URL hasn't changed, we skip the ~30 s panel refresh on this wake. That's ~0.6 mAh saved per hash-skip wake.
- **WiFi off before painting** — `wifi_sta_stop()` (and the MQTT client) are torn down *before* `epd_init()` so we don't burn ~80 mA holding the radio during the multi-second panel render. This is the single biggest battery saving in the render path.
- **Panel power gated by GPIO** — `EPD_PIN_PWR` (GPIO1) is high only during refresh; `epd_sleep()` drops it.

## Hardware

| Component | Detail |
| --- | --- |
| Module | ESP32-S3-WROOM-2-N32R16V (32 MB flash, 16 MB octal PSRAM) |
| Panel | 13.3" Spectra E6, 1200×1600 native portrait, 6 colours, 4 bpp packed |
| Battery | Single-cell Li-Po (3.3–4.2 V) via the onboard ETA6098 charger; battery sense on GPIO8 / ADC1 ch7 through a 1:3 divider |

Pinout (see [include/app_config.h](include/app_config.h)) matches the official Waveshare ESP-IDF demo. The panel has two SPI chip-selects (`CS_M`, `CS_S`) which drive the left and right halves of the display independently.

### Power profile

| State | Current | Duration |
|---|---|---|
| WiFi + MQTT + HTTP burst | ~80 mA | 5–15 s |
| Panel refresh | ~60 mA | ~30 s (only on render wake) |
| Deep sleep | single-digit µA | the rest of the time |

Per-wake cost on a 4 V Li-Po, default 15 min sleep interval:

- **Hash-skip wake** (no render): ~0.13 mAh
- **Render wake** (download + paint): ~0.75 mAh
- **Deep sleep idle**: ~1.2 mAh/day

Expected runtime by usage:

| Use case | Daily draw | 2000 mAh | 5000 mAh | 10000 mAh |
|---|---|---|---|---|
| Photo frame (1 update/day) | ~14 mAh | 5 months | **12 months** | 24 months |
| Dashboard (hourly updates) | ~30 mAh | 2 months | **5.5 months** | 11 months |
| Frequent (every wake renders) | ~75 mAh | 1 month | 2 months | 4 months |

For "months of usage" with a typical update cadence, a 5000–10000 mAh single-cell Li-Po pouch is the sweet spot. Bumping `sleep_interval_s` via the config topic from 15 min to 30 or 60 min proportionally cuts wake-cycle cost.

## Wake cycle

```
boot
  ├─ cold boot (RESET / power-on)?   → paint 6-band colour splash (panel sanity check)
  ├─ no WiFi creds anywhere?         → captive portal → reboot
  ├─ STA connect fails?              → captive portal → reboot
  ├─ publish heartbeat               (battery_mv/pct, rssi, ip, fw_version)
  ├─ no retained MQTT message?       → sleep (nothing new to show)
  ├─ URL unchanged since last wake?  → sleep (skip ~30 s refresh, save ~0.6 mAh)
  ├─ fetch URL → validate length → paint
  ├─ free WiFi BEFORE panel refresh  (saves ~80 mA during the 30 s refresh)
  └─ deep sleep OR loop:
       ├─ USB host attached?         → short-delay restart loop (dev mode)
       └─ otherwise                  → deep sleep for the configured interval
```

The "no sleep when USB-host attached" logic uses ESP-IDF's `usb_serial_jtag_is_connected()` to read host SOF packets — a USB charger / power bank (no SOFs) is treated as battery operation. Force-override either way with `DEV_DISABLE_SLEEP` in `secrets.h`.

The cold-boot splash fires only on `ESP_RST_POWERON` / `ESP_RST_EXT` — burning 30 s on every timer wake would halve battery life.

Behaviour tunables (sleep interval, retry counts, AP credentials, MQTT topics / broker URI) all live at the top of [include/app_config.h](include/app_config.h).

## Build & flash

Requires [PlatformIO](https://platformio.org/). ESP-IDF 5.x and the Xtensa toolchain are pulled automatically on first build.

```bash
pio run                                          # build
pio run -e tesserae-esp32-bin-client -t upload   # flash via USB
pio device monitor                               # 115200 baud, exception decoder enabled
```

First boot brings up a SoftAP named `Tesserae-Setup` (password `tesserae`). Join it from your phone; the captive-portal prompt opens a form for your home WiFi credentials and MQTT broker. After submit the device reboots, joins your network, and enters the normal wake cycle.

### Dev shortcut: `secrets.h`

To skip the captive portal during iteration, copy [include/secrets.example.h](include/secrets.example.h) to `include/secrets.h` and uncomment the `WIFI_DEFAULT_*` / `MQTT_DEFAULT_*` macros you want baked into the build. `secrets.h` is git-ignored. Precedence on each wake is NVS (set via portal) → `secrets.h` values → empty (portal triggers).

For fast iteration without USB plugged in (e.g. headless testing), also define `DEV_DISABLE_SLEEP` in `secrets.h` — the firmware will loop on `DEV_LOOP_INTERVAL_S` (default 10 s) instead of deep-sleeping. With USB plugged in this is automatic; the manual flag is only needed otherwise.

## MQTT contract

The firmware uses three topics under the `tesserae/esp32/` namespace. The frame and config topics are read on every wake; the status topic is written on every wake.

| Topic | Direction | Retained | QoS | Purpose |
|---|---|---|---|---|
| `tesserae/esp32/frame/bin` | server → device | yes | 1 | URL of the next `.bin` frame to render |
| `tesserae/esp32/config` | server → device | yes | 1 | Runtime device settings (sleep interval) |
| `tesserae/esp32/status` | device → broker | yes | 1 | Wake-time heartbeat + LWT |

All three topic names are overridable in `secrets.h` (`MQTT_DEFAULT_TOPIC`, `MQTT_DEFAULT_CONFIG_TOPIC`, `MQTT_DEFAULT_STATUS_TOPIC`). The frame topic is additionally runtime-overridable via the captive portal (stored in NVS); the config and status topics are compile-time only.

The client also registers a **last-will-and-testament** on `tesserae/esp32/status` with `{"state":"offline"}` retained. The broker publishes that on ungraceful disconnect (keepalive timeout, TCP drop) so Tesserae can flag a probably-dead-battery device. On the next normal wake the full heartbeat overwrites the offline marker.

### Frame payload

JSON pointing to a panel-native `.bin` artifact:

```json
{ "url": "http://192.168.1.10:8000/renders/3f7a91b2c4e5d6f8.bin" }
```

A bare URL string (anything starting with `http://` or `https://`) is also accepted — useful for testing with `mosquitto_pub` directly.

**Wire format** of the file at that URL: raw, headerless, no magic bytes, no length prefix, no checksum. Exactly **960,000 bytes** (`1200 × 1600 / 2`), scanline order, no row padding, two pixels per byte where the **high nibble holds the even column** (cols 0, 2, 4 …) and the low nibble the odd column.

Palette nibble values (firmware-reserved `0x4` and `0x7` are never written):

| Nibble | Colour |
|---|---|
| `0x0` | Black |
| `0x1` | White |
| `0x2` | Yellow |
| `0x3` | Red |
| `0x5` | Blue |
| `0x6` | Green |

The buffer is panel-native — no rotation, no decode, no resize required. The left half of each row (300 bytes, columns 0–599) is streamed to the left controller (`CS_M`); the right half (columns 600–1199) to the right controller (`CS_S`) — see `epd_display` in [src/epd_driver.c](src/epd_driver.c).

If the downloaded body isn't exactly 960,000 bytes the firmware logs a `frame size mismatch` error and goes back to sleep without painting. The Tesserae server's `esp32_bin` renderer always produces this exact length; anything else is a server-side bug and feeding the panel garbage costs ~30 s of refresh power for nothing.

The firmware persists the SHA-256 of the rendered URL in NVS so unchanged retained messages don't trigger needless panel refreshes.

### Config payload

JSON; applied on next wake and persisted to NVS so it survives reboots.

```json
{ "sleep_interval_s": 900 }
```

Clamped to `[30, 604800]` (30 s – 7 days). Out-of-range values are rejected with a log warning and the previously-stored interval is kept. Falls back to the compile-time `SLEEP_INTERVAL_S` (15 min) when NVS is empty.

### Status (heartbeat) payload

Published once per wake, immediately after the broker connection succeeds and **before** the URL fetch / panel paint:

```json
{
  "battery_mv": 3950,
  "battery_pct": 67,
  "rssi": -42,
  "ip": "192.168.50.234",
  "fw_version": "0.1.0"
}
```

- `battery_mv` — millivolts at the cell. `0` means the ADC read failed (treat as unknown, not flat).
- `battery_pct` — 0–100, derived from a two-segment piecewise-linear Li-Po curve.
- `rssi` — wifi signal in dBm at the time of the heartbeat.
- `ip` — STA IPv4 address.
- `fw_version` — firmware version string from [include/app_config.h](include/app_config.h).

Retained, so Tesserae can show "last known state" without the device being awake. A heartbeat significantly older than ~3× the configured sleep interval is a probably-dead-battery signal — Tesserae surfaces these in its UI.

### Manual test push

```bash
mosquitto_pub -t tesserae/esp32/frame/bin -r \
  -m '{"url":"http://192.168.1.10:8000/renders/test.bin"}'
```

The next wake will fetch and paint that frame.

## Troubleshooting

- **No captive portal on first boot** — button-mash the RESET button. The portal triggers when WiFi creds aren't present in NVS, so a fresh board (or one wiped with `idf.py erase-flash`) brings it up automatically. The AP is `Tesserae-Setup` (password `tesserae`).
- **STA connects but no paint** — broker unreachable from the ESP32's IP. Check `ip` in the latest retained heartbeat on `tesserae/esp32/status` and ping the broker URI from that subnet. Common causes: broker bound to `127.0.0.1`, firewall, VLAN isolation.
- **Paint starts and stops** — `frame size mismatch` in the serial log means the URL served something that isn't 960,000 bytes. Verify with `curl -sI <url>` that `Content-Length: 960000`.
- **Splash colours look wrong** — panel ribbon seated badly or wrong panel rev. The init byte sequence in [src/epd_driver.c](src/epd_driver.c) is panel-specific and must not be modified.
- **Battery drains in days, not months** — usually means WiFi is being held during the panel refresh. Confirm `wifi_sta_stop()` runs *before* `epd_init()` in [src/main.c](src/main.c). On a USB power meter, the 30 s refresh phase should sit around 60 mA, not 140 mA.

## Project layout

```
tesserae-esp32-bin-client/
├── platformio.ini             # board, partitions, monitor settings
├── partitions.csv             # 14 MB factory app + NVS
├── sdkconfig.defaults         # PSRAM, mbedTLS cert bundle, MQTT 3.1.1
├── include/
│   ├── app_config.h           # pinout + all behaviour tunables + FW_VERSION
│   ├── secrets.example.h      # template for local credential overrides
│   └── secrets.h              # (git-ignored) your local overrides
└── src/
    ├── main.c                 # boot → splash → connect → render → sleep
    ├── epd_driver.{c,h}       # Waveshare 13.3E6 panel driver + colour-bar splash
    ├── heartbeat.{c,h}        # battery / RSSI / IP / fw_version JSON formatter
    ├── wifi_manager.{c,h}     # NVS-backed STA connect with retry
    ├── provisioning.{c,h}     # SoftAP + DNS hijack + HTTP form captive portal
    ├── mqtt_config.{c,h}      # NVS-backed broker URI / topic / credentials
    ├── mqtt_handler.{c,h}     # single-shot subscribe + dispatch + heartbeat + LWT
    ├── image_fetcher.{c,h}    # HTTP download into PSRAM
    └── image_decoder.{c,h}    # strict 960000-byte panel-native pass-through
```

No tests — smoke-testing on real hardware is the workflow. Recommended validation after any change to the wake state machine:

1. Flash a fresh board, walk it through the captive portal.
2. `mosquitto_pub -t tesserae/esp32/frame/bin -r -m '{"url":"http://.../test.bin"}'` and confirm the panel paints.
3. Pull battery, attach to a USB current meter, and log average draw over 24 h. Anything above ~35 mAh/day for the photo-frame use case means WiFi is leaking on somewhere.

## Credits

The panel driver in [src/epd_driver.c](src/epd_driver.c) is a port of the official ESP-IDF demo published by Waveshare at [waveshareteam/ESP32-S3-ePaper-13.3E6](https://github.com/waveshareteam/ESP32-S3-ePaper-13.3E6). The init byte sequence and command set are panel-specific and kept byte-for-byte exact.

The wake state machine, captive-portal provisioning, NVS schema, and battery curve are forked from the earlier `esp32-inky-dash-client` project; the same hardware substrate runs both.

## License

MIT — see [LICENSE](LICENSE).
