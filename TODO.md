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
