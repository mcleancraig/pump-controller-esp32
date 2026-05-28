# TODO — v0.1.0 and beyond

## UX

- [ ] **Reconfigure page** — add a `/reconfigure` endpoint (or an MQTT-triggered
  portal mode) that starts the captive portal pre-populated with the current NVS
  values, so any field (WiFi, static IP, MQTT, pumps) can be changed without a
  factory reset. Currently the only way to change config post-deployment is via
  individual HA entities or a full wipe-and-re-pair.

## Security

- [ ] **FOTA TLS certificate validation** — `checkForUpdate()` uses
  `client.setInsecure()`. Embed the ISRG Root X1 CA cert and call
  `client.setCACert()` instead.

- [ ] **Per-device AP password** — derive the portal password from the last 3 bytes
  of the MAC address instead of the shared `pumpcontroller` string.

## Nice to have

- [ ] **Syslog boot-message replay** — currently boot messages are discarded in
  `syslogFlush()` to avoid the ~4 s-per-packet ARP block on a cold cache. Once the
  ARP cache is warm (a few seconds after WiFi connects) a background task could
  re-send them, so syslog gets a complete record of every boot.

- [ ] **FOTA downgrade protection** — parse semver and only flash if remote >
  local; downgrade opt-in only.

## v2.0 hardware

- [ ] **Per-outlet NeoPixel ring lights** — WS2812B ring around each pump outlet,
  animated (rolling blue) when that pump is active. One data line per ring (or
  chained with offsets). Note: adds meaningful BOM cost per unit.
  - Reference: [Adafruit #2853 — 12x RGBW NeoPixel Ring](https://www.adafruit.com/product/2853) ($9.50, 36.8mm OD / 23.3mm ID)
  - Cheaper: [Adafruit #1643 — 12x RGB NeoPixel Ring](https://www.adafruit.com/product/1643) ($8.95, same dims, RGB not RGBW — simpler library)
  - AliExpress clones available ~$1–2/unit; search "WS2812B 12 LED ring"
