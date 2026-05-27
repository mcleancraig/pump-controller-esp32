# pump-controller-esp32

Firmware for a WiFi-connected garden pump controller based on the **Waveshare ESP32-C6-Zero**. Supports up to 5 independently controlled pumps, a VL53L0X distance sensor for water level monitoring, a piezo buzzer for alerts, MQTT control, Home Assistant autodiscovery, and over-the-air firmware updates.

## Features

- **Up to 5 pumps** — each on a configurable GPIO, independently triggered via MQTT
- **Hardware safety cap** — maximum pump run time enforced in firmware, not overridable via config or MQTT
- **Water level sensor** — VL53L0X ToF distance sensor measures tank fill %; blocks and stops pumps when empty threshold is reached
- **Piezo buzzer** — audio alerts for watering started, watering done, low water, and boot
- **Captive portal** — first-boot WiFi and full device configuration via web browser
- **MQTT** — per-pump command/state topics; unit heartbeat; availability LWT; config get/set per field
- **Home Assistant autodiscovery** — pumps, water level sensor, piezo config, and all config fields appear automatically in HA
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
| Pump switching | IRLZ44N N-channel MOSFET (logic-level, 55V/47A) |
| Water level | VL53L0X ToF distance sensor on I2C (Wire: SDA=GPIO14, SCL=GPIO15) |
| Piezo | Passive buzzer driven via LEDC PWM |
| Supply | 12V or 24V for pumps; ESP32 powered separately via USB or 3.3V rail |

### Default GPIO assignments

| GPIO | Function |
|---|---|
| 1–5 | Pump 1–5 outputs (default; configurable) |
| 9 | Boot button |
| 14 | I2C SDA — VL53L0X water level sensor |
| 15 | I2C SCL — VL53L0X water level sensor |
| 21 | Piezo buzzer (default; configurable) |

### Pump wiring (IRLZ44N)

```
ESP32 GPIO ──[330Ω]── Gate
                      Drain ──── Pump − (motor negative)
                      Source ─── GND (common with ESP32 GND)

12V/24V ───────────────────────── Pump +
Flyback diode across pump terminals (cathode/stripe to supply rail)
```

### Water level sensor wiring (VL53L0X)

```
VL53L0X    ESP32-C6-Zero
VIN    →   3.3V
GND    →   GND
SDA    →   GPIO14
SCL    →   GPIO15
```

Mount the sensor at the top of the reservoir pointing down. Configure `waterFullMm` (distance when full, e.g. 25 mm) and `waterEmptyMm` (distance when empty / pumps stop, e.g. 190 mm) via the portal or HA.

### Piezo buzzer wiring

```
Piezo + → GPIO21 (or configured pin)
Piezo − → GND
```

Passive buzzer only — active buzzers will not produce the correct tones.

## First boot / configuration

1. Flash the firmware (see [Build & Flash](#build--flash))
2. On first boot the device starts a WiFi access point: **`PumpController-XXXX`** password **`pumpcontroller`**
3. Connect to the AP — your device should be redirected automatically to the config portal, otherwise browse to `192.168.4.1`
4. Fill in WiFi credentials, MQTT broker, pump count, GPIO pins, and optionally enable the water level sensor and piezo buzzer
5. Save — the device restarts and connects

To reconfigure: hold the boot button for 3 seconds to restart into the portal, or wipe NVS via the HA Reset Config button.

## MQTT topics

All topics are prefixed with `garden/pumpN/` where N is the unit number set in the portal.

| Topic | Direction | Payload | Notes |
|---|---|---|---|
| `garden/pump1/availability` | publish | `online` / `offline` | Retained; LWT publishes `offline` on disconnect |
| `garden/pump1/state` | publish | JSON | Heartbeat every 30 s |
| `garden/pump1/pump/N/cmd` | subscribe | `water` / `stop` | Trigger or stop pump N |
| `garden/pump1/pump/N/state` | publish | `running` / `idle` | Retained pump state |
| `garden/pump1/water_level` | publish | JSON | Retained; published every poll and on state change |
| `garden/pump1/config/set/+` | subscribe | raw value | Set a single config field by name (see below) |
| `garden/pump1/config/state` | publish | JSON | Full config; published on connect and after changes |
| `garden/pump1/cmd` | subscribe | `restart` / `reset` | Restart or factory-reset the device |

### Water level payload example

```json
{"water_level": "OK", "water_pct": 78}
```

`water_level` is `LOW` when `waterEmptyMm` threshold is reached. All running pumps stop immediately and new starts are blocked until level recovers.

### Per-field config set

Publish a raw value (not JSON) to `garden/pump1/config/set/<field>`:

| Field | Type | Example payload | Notes |
|---|---|---|---|
| `pumpDuration0` | integer (seconds) | `30` | 0-indexed |
| `pumpPin0` | integer (GPIO) | `3` | 0-indexed |
| `waterLevelPin` | `ON` / `OFF` | `ON` | Enables/disables sensor |
| `waterFullMm` | integer (mm) | `25` | Distance when tank is full |
| `waterEmptyMm` | integer (mm) | `190` | Distance when tank is empty |
| `piezoPin` | integer (GPIO) | `21` | `-1` to disable |
| `mqttBroker` | string | `192.168.1.10` | |
| `staticIP` | `ON` / `OFF` | `ON` | |
| `fwChannel` | string | `beta` | `stable` or `beta` |

## Home Assistant

Entities appear automatically via MQTT autodiscovery under the device **"Garden Pump Controller N"**:

- **Switch** per pump — starts/stops with configured duration
- **Binary sensor** — Water Level (`device_class: problem`, ON = LOW)
- **Sensor** — Water Level % (0–100, updates every 2 s)
- **Switch** — Water Sensor enable/disable
- **Number** — Water Full Distance (mm) / Water Empty Distance (mm)
- **Number** — Piezo Buzzer Pin, Pump GPIO pins, Pump durations
- **Switch** — Static IP enable
- **Text** — IP / gateway / subnet / DNS / MQTT broker / syslog host
- **Select** — FOTA channel (`stable` / `beta`)
- **Button** — Restart / Factory Reset
- **Sensor** — Firmware version

## FOTA

The device checks `https://github.com/mcleancraig/pump-controller-esp32/releases/latest/download/version.txt` hourly. If the remote version is newer it downloads and flashes `pump-controller-esp32.ino.bin` from the same release.

Set `fwChannel` to `beta` via MQTT or HA to track pre-release builds instead.

## Build & Flash

Requires [arduino-cli](https://arduino.github.io/arduino-cli/) and the `esp32:esp32` platform. Also requires the **Pololu VL53L0X** library (`arduino-cli lib install "VL53L0X"`).

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
| Device reboots every 60 s | Watchdog firing — loop stalled | Check WiFi/MQTT connectivity |
| Pumps blocked, won't start | Water level LOW or sensor read failure | Check VL53L0X wiring; inspect `water_level` topic |
| `VL53L0X init FAILED` in log | Wiring issue | Check SDA=GPIO14, SCL=GPIO15; confirm 3.3V supply |
| Water % always 0 | `waterEmptyMm` ≤ `waterFullMm` | Reconfigure distances via HA or portal |
| Pump always on | MOSFET gate floating or shorted | Confirm 330Ω gate resistor; check FET orientation (G-D-S) |
| No buzzer tones | Wrong pin or active buzzer | Confirm passive buzzer; check `piezoPin` config |

## Licence

[MIT](LICENSE)
