# Changelog

All notable changes to pump-controller-esp32 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.1.0] — 2026-05-16

### Changed
- State payload field `"firmware_version"` renamed to `"fw_version"` — update any HA value templates that reference `value_json.firmware_version`
- MQTT client ID changed from `pump-ctrl-N` to `garden-pumpN` for cross-repo consistency
- Config set topic changed from single JSON topic `garden/pumpN/config/set` to per-field wildcard `garden/pumpN/config/set/+` — each field is now set individually (e.g. `garden/pump1/config/set/pumpDuration1`)
- FOTA version comparison now uses integer semver parsing — fixes `"2.10.0" < "2.9.0"` bug under lexicographic `strcmp`
- Beta channel FOTA now uses strict `isNewerVersion()` instead of `!=` — prevents downgrade if remote beta is older
- Water level payload changed from plain string `OK`/`LOW` to JSON `{"water_level":"OK"}`/`{"water_level":"LOW"}` — update HA value templates accordingly
- HA discovery config entities now use per-field `command_topic` (`garden/pumpN/config/set/fieldName`) instead of `command_template` + single topic

### Added
- MQTT LWT — `garden/pumpN/availability` publishes `offline` on disconnect, `online` on connect (retained)
- Hardware watchdog — 60 s WDT via `esp_task_wdt`; reset on each loop iteration
- WiFi reconnect in `loop()` — reconnects if association drops without requiring a full reboot
- `isNewerVersion()` helper for integer semver comparison
- Reset reason logged at boot via `esp_reset_reason()`
- Heap monitoring: free + minimum free heap logged after WiFi connects and after FOTA check
- `build.sh` — one-command arduino-cli compile script
- `CHANGELOG.md`, `.gitignore`, `LICENSE`

## [1.0.3]

### Fixed
- FOTA binary filename corrected

## [1.0.2]

### Fixed
- WiFi static IP config applied before connection attempt

## [1.0.1]

### Changed
- Stop all running pumps immediately when water level transitions to LOW

## [1.0.0]

### Added
- Water level sensor: NO reed switch + float/magnet with 2 s debounce
- Pump start blocked and running pumps stopped when water is LOW
- HA `binary_sensor` (device_class: problem) for water level
- GPIO conflict detection at boot (warnings) and on save (rejected)
- Water level pin configurable via portal and HA number entity

## [0.1.0]

### Added
- Initial release: configurable N pumps (1–5), MQTT, captive portal, syslog, HA autodiscovery, FOTA, NTP, NVS config
- Hard firmware safety cap on pump run time
- Static IP / gateway / subnet / DNS configurable from HA

[1.1.0]: https://github.com/mcleancraig/pump-controller-esp32/releases/tag/v1.1.0
