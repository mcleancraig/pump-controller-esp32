# pump-controller-esp32

Firmware for a WiFi-connected garden pump controller based on the **Waveshare ESP32-C6-Zero**. Supports up to 5 independently controlled pumps, an optional water level sensor, MQTT control, Home Assistant autodiscovery, and over-the-air firmware updates.

## Features

- **Up to 5 pumps** — each on a configurable GPIO, independently triggered via MQTT
- **Hardware safety cap** — maximum pump run time enforced in firmware, not overridable via config or MQTT
- **Water level sensor** — NO reed switch + float/magnet detects low tank; blocks and immediately stops pumps when triggered
- **Captive portal** — first-boot WiFi and full device configuration via web browser
- **MQTT** — per-pump command/state topics; unit heartbeat; availability LWT; config get/set per field
- **Home Assistant autodiscovery** — pumps, water level binary sensor, and all config fields appear automatically in HA
- **FOTA** — checks GitHub Releases hourly; self-updates when a newer version is available; optional beta channel
- **Syslog** — structured log output to a UDP syslog server
- **NTP** — clock sync at boot for accurate log timestamps
- **Static IP support** — optional, configurable via portal or HA entities
- **Hardware watchdog** — 60 s WDT resets device if main loop stalls
- **WiFi reconnect** — automatic reassociation without reboot if connection drops

## Hardware

| Component | Details |
|---|---|
| MCU | Waveshare ESP32-C6-Zero |
| Pump switching | IRLZ44N MOSFET (recommended) or 2N2222 for low-current loads |
| Water level | NO reed switch between GPIO and GND (`INPUT_PULLUP`) |
| Supply | 5 V USB or 12 V via PD board + buck converter to 3.3 V |

### Default GPIO assignments

| GPIO | Function |
|---|---|
| 1–5 | Pump 1–5 outputs (default; configurable) |
| 9 | Boot button |
| 20 | Recommended for water level reed switch |

### Pump wiring (IRLZ44N)

```
ESP32 GPIO ──── 330Ω ──── Gate
                           Drain ──── Pump − (motor negative)
                           Source ─── GND
12V ───────────────────────────────── Pump +
Flyback diode across pump (cathode to 12V)
```

### Water level sensor wiring

```
GPIO20 ──── Reed switch ──── GND
```

`INPUT_PULLUP` holds the line HIGH (water OK). Float drops when tank is low → magnet closes reed → GPIO LOW = water LOW. All running pumps stop immediately; new starts are blocked until level recovers.

## First boot / configuration

1. Flash the firmware (see [Build & Flash](#build--flash))
2. On first boot the device starts a WiFi access point: **`PumpController-N`** password **`pumpcontroller`**
3. Connect to the AP — your device should be redirected automatically to the config portal, otherwise browse to `192.168.4.1`
4. Fill in WiFi credentials, MQTT broker, pump count, GPIO pins, and (optionally) the water level sensor pin
5. Save — the device restarts and connects

To reconfigure: hold the boot button for 3 seconds to restart into the portal, or 10 seconds to wipe NVS and start fresh.

## MQTT topics

All topics are prefixed with `garden/pumpN/` where N is the unit number set in the portal.

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `garden/pump1/availability` | publish | `online` / `offline` | Retained; LWT publishes `offline` on disconnect |
| `garden/pump1/state` | publish | JSON | Heartbeat every 30 s |
| `garden/pump1/pump/N/cmd` | subscribe | `water` / `stop` | Trigger or stop pump N |
| `garden/pump1/pump/N/state` | publish | `ON` / `OFF` | Retained pump state |
| `garden/pump1/water_level` | publish | JSON | Retained; published on change and reconnect |
| `garden/pump1/config/set/+` | subscribe | raw value | Set a single config field by name (see below) |
| `garden/pump1/config/state` | publish | JSON | Full config; published on connect and after changes |
| `garden/pump1/cmd` | subscribe | `restart` / `reset` | Restart or factory-reset the device |

### State payload example

```json
{
  "fw_version": "1.1.0",
  "uptime": 3600,
  "rssi": -62,
  "pump1": false,
  "pump2": false
}
```

### Water level payload example

```json
{"water_level": "OK"}
```

### Per-field config set

Publish a raw value (not JSON) to `garden/pump1/config/set/<field>`:

| Field | Type | Example topic | Example payload |
|---|---|---|---|
| `pumpDuration1` | integer (seconds) | `garden/pump1/config/set/pumpDuration1` | `30` |
| `pumpPin1` | integer (GPIO) | `garden/pump1/config/set/pumpPin1` | `3` |
| `waterLevelPin` | integer (GPIO) | `garden/pump1/config/set/waterLevelPin` | `20` |
| `mqttBroker` | string | `garden/pump1/config/set/mqttBroker` | `192.168.1.10` |
| `staticIP` | boolean | `garden/pump1/config/set/staticIP` | `ON` / `OFF` |
| `fwChannel` | string | `garden/pump1/config/set/fwChannel` | `beta` |

## Home Assistant

Entities appear automatically via MQTT autodiscovery under the device **"Garden Pump Controller N"**:

- **Switch** per pump (starts/stops with configured duration)
- **Binary sensor** — Water Level (`device_class: problem`, ON = LOW); update value template to `{{ value_json.water_level }}`
- **Number** entities for all config fields (pump pins, durations, water level pin, etc.)
- **Switch** for static IP enable
- **Text** entities for IP / gateway / subnet / DNS / MQTT broker / syslog host
- **Select** — FOTA channel (`stable` / `beta`)
- **Button** — Restart / Factory Reset / Firmware Update
- **Sensor** — Firmware version; uses `value_json.fw_version`

### Availability

All entities use `garden/pumpN/availability` with `payload_available: online` and `payload_not_available: offline`. The device publishes `online` on MQTT connect and `offline` via LWT on unexpected disconnect.

## FOTA

The device checks `https://github.com/mcleancraig/pump-controller-esp32/releases/latest/download/version.txt` hourly. If the remote version is newer (integer semver comparison) it downloads and flashes `pump-controller-esp32.ino.bin` from the same release.

Set `fwChannel` to `beta` via MQTT or HA to track pre-release builds instead.

## Build & Flash

Requires [arduino-cli](https://arduino.github.io/arduino-cli/) and the `esp32:esp32` platform.

> **Important:** the Waveshare ESP32-C6-Zero uses USB CDC. Always include `CDCOnBoot=cdc` in the FQBN or serial output will be silently routed to UART0 and nothing will appear on the monitor.

```bash
# Compile
./build.sh

# Upload
arduino-cli upload -p /dev/cu.usbmodem* \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  build/pump-controller-esp32.ino.bin
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| No serial output | CDCOnBoot not set | Recompile with `CDCOnBoot=cdc` in FQBN |
| Device reboots every 60 s | Watchdog firing — loop stalled | Check WiFi/MQTT connectivity; syslog host reachable? |
| Pumps blocked, won't start | Water level LOW | Check float / reed switch; inspect `water_level` topic |
| Config set has no effect | Old single-topic approach | Publish to `config/set/<field>` not `config/set` |
| HA shows firmware as `fw` | Old `value_json.fw` template | Update template to `value_json.fw_version` |
| FOTA not triggering at ≥ v2.10 | Lexicographic version compare | Upgrade to v1.1.0+ which uses integer semver |

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Licence

[MIT](LICENSE)
