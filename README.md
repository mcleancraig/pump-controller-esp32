# pump-controller-esp32

Firmware for a WiFi-connected garden pump controller based on the **Waveshare ESP32-C6-Zero**. Supports up to 5 independently controlled pumps, an optional water level sensor, MQTT control, Home Assistant autodiscovery, and over-the-air firmware updates.

## Features

- **Up to 5 pumps** — each on a configurable GPIO, independently triggered via MQTT
- **Hardware safety cap** — maximum pump run time enforced in firmware, not overridable via config or MQTT
- **Water level sensor** — NO reed switch + float/magnet detects low tank; blocks and immediately stops pumps when triggered
- **Captive portal** — first-boot WiFi and full device configuration via web browser
- **MQTT** — per-pump command/state topics; unit heartbeat; config get/set
- **Home Assistant autodiscovery** — pumps, water level binary sensor, and all config fields appear automatically in HA
- **FOTA** — checks GitHub Releases hourly; self-updates when a newer version is available
- **Syslog** — structured log output to a UDP syslog server
- **NTP** — clock sync at boot for accurate log timestamps
- **Static IP support** — optional, configurable via portal or HA entities

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
| `garden/pump1/state` | publish | JSON | Heartbeat every 30 s — firmware version, uptime, RSSI, pump states |
| `garden/pump1/pump/N/cmd` | subscribe | `water` / `stop` | Trigger or stop pump N |
| `garden/pump1/pump/N/state` | publish | `ON` / `OFF` | Retained pump state |
| `garden/pump1/water_level` | publish | `OK` / `LOW` | Retained; published on change and reconnect |
| `garden/pump1/config/set` | subscribe | JSON | Update any config field at runtime |
| `garden/pump1/config/state` | publish | JSON | Full config state; published on connect and after changes |
| `garden/pump1/cmd` | subscribe | `restart` / `reset` | Restart or factory-reset the device |

## Home Assistant

Entities appear automatically via MQTT autodiscovery under the device **"Garden Pump Controller N"**:

- **Switch** per pump (starts/stops with configured duration)
- **Binary sensor** — Water Level (`device_class: problem`, ON = LOW)
- **Number** entities for all config fields (pump pins, durations, water level pin, MQTT, syslog, etc.)
- **Switch** for static IP enable
- **Text** entities for IP / gateway / subnet / DNS
- **Button** — Restart / Factory Reset
- **Sensor** — Firmware version

## FOTA

The device checks `https://github.com/mcleancraig/pump-controller-esp32/releases/latest/download/version.txt` every hour. If the remote version differs from the running firmware it downloads and flashes `pump-controller-esp32.ino.bin` from the same release.

## Build & Flash

Requires [arduino-cli](https://arduino.github.io/arduino-cli/) and the `esp32:esp32` platform.

> **Important:** the Waveshare ESP32-C6-Zero uses USB CDC. Always include `CDCOnBoot=cdc` in the FQBN or serial output will be silently routed to UART0 and nothing will appear on the monitor.

```bash
# Compile
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  --output-dir /tmp/pump-build \
  pump-controller-esp32.ino

# Upload
arduino-cli upload -p /dev/cu.usbmodem* \
  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc \
  /tmp/pump-build/pump-controller-esp32.ino.bin
```

## Changelog

### v1.0.1
- Stop all running pumps immediately when water level transitions to LOW

### v1.0.0
- Water level sensor: NO reed switch + float/magnet with 2 s debounce
- Pump start blocked and running pumps stopped when water is LOW
- HA `binary_sensor` (device_class: problem) for water level
- GPIO conflict detection at boot (warnings) and on save (rejected)
- Water level pin configurable via portal and HA number entity

### v0.1.0
- Initial release: configurable N pumps (1–5), MQTT, captive portal, syslog, HA autodiscovery, FOTA, NTP, NVS config
- Hard firmware safety cap on pump run time
- Static IP / gateway / subnet / DNS configurable from HA
