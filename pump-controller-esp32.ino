
#include <WiFi.h>
#include <WiFiUDP.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_mac.h"

// ═══════════════════════════════════════════════════════════
//  v1.0.2
//  - Fix pin conflict false positives: scope applyConfigUpdate() checks to
//    cfg.pumpCount not MAX_PUMPS (inactive slots hold stale default GPIOs)
//  - Add raw pin check in pump auto-stop loop for immediate stop on water LOW
//    (no debounce wait when a pump is running)
//  - Default static IP: GW + DNS -> 192.168.1.1; IP -> 192.168.211.<unit>
//  - Dual-channel FOTA: "stable" (default) and "beta" (-b01..99 tags)
//    selectable from HA; beta uses GitHub API to find pre-release tags
//  - fwChannel stored in NVS; exposed as HA select entity
//
//  v1.0.1
//  - Stop all running pumps immediately when water level transitions to LOW
//
//  v1.0.0
//  - Water level sensor: NO reed switch + float/magnet. Float drops when
//    tank is low → magnet closes reed → GPIO LOW = water LOW. Reed open
//    (INPUT_PULLUP HIGH) = water OK. 2-second debounce. HA binary_sensor
//    (device_class: problem). Pump start blocked when water is LOW.
//    GPIO conflict detection at boot (warnings) and on save (rejected).
//    Pin configurable via portal and HA number entity.
//
//  v0.1.0
//  - NVS magic key: loadConfig() checks for "pump-ctrl-1" magic in the
//    "pump" namespace; if absent or wrong (stale NVS from a different
//    firmware), namespace is cleared and device falls through to portal.
//    Stricter than moisture sensor — no deployed devices to be backwards
//    compatible with, so missing magic is treated as stale.
//  - Initial release: configurable N pumps (1-5), always-awake loop,
//    MQTT command/state per pump, WiFi+captive portal, syslog,
//    HA MQTT autodiscovery, FOTA, NTP, NVS config
//  - Hard safety cap: PUMP_MAX_DURATION_S — firmware-enforced, not
//    overridable via config or MQTT
//  - MQTT callback safety: no publish() inside callback; deferred via flags
// ═══════════════════════════════════════════════════════════

#define FIRMWARE_VERSION "1.0.2"

// ── Hardware constants ────────────────────────────────────
const int BTN_BOOT  = 9;      // Boot button — GPIO9 on Waveshare C6-Zero / XIAO C6
const int MAX_PUMPS = 5;
const int DEFAULT_PUMP_PINS[MAX_PUMPS] = { 1, 2, 3, 4, 5 };

// ── Safety cap ────────────────────────────────────────────
// Firmware-enforced maximum run time. Cannot be raised via config or MQTT.
const int PUMP_MAX_DURATION_S = 30;

// ── Timing ────────────────────────────────────────────────
const int AP_TIMEOUT_MIN            = 10;
const int BOOT_HOLD_MS              = 3000;
const int WIFI_TIMEOUT_MS           = 15000;
const int MQTT_TIMEOUT_S            = 5;
const unsigned long MQTT_RECONNECT_COOLDOWN_MS = 5000;
const unsigned long HEARTBEAT_INTERVAL_MS      = 30000;
const unsigned long FOTA_CHECK_INTERVAL_MS     = 3600000UL;  // 1 hour
const unsigned long WATER_DEBOUNCE_MS          = 2000;       // reed switch debounce
const int NTP_TIMEOUT_MS            = 10000;
const int FOTA_VERSION_TIMEOUT_MS   = 8000;
const int FOTA_DL_TIMEOUT_MS        = 60000;

// ── AP credentials ────────────────────────────────────────
const char* AP_PASSWORD = "pumpcontroller";

// ── NTP ───────────────────────────────────────────────────
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 0;
const int   DST_OFFSET_S = 0;

// ── FOTA ─────────────────────────────────────────────────
// Stable channel: version.txt + binary from /releases/latest/download/
const char* FOTA_VERSION_URL =
  "https://github.com/mcleancraig/pump-controller-esp32"
  "/releases/latest/download/version.txt";
const char* FOTA_BIN_URL =
  "https://github.com/mcleancraig/pump-controller-esp32"
  "/releases/latest/download/pump-controller-esp32.ino.bin";
// Beta channel: GitHub API /releases?per_page=1 returns the newest release
// including pre-releases (which /releases/latest skips).
const char* FOTA_RELEASES_API_URL =
  "https://api.github.com/repos/mcleancraig/pump-controller-esp32"
  "/releases?per_page=1";
// Binary URL template for beta — tag substituted at runtime.
const char* FOTA_BIN_URL_TMPL =
  "https://github.com/mcleancraig/pump-controller-esp32"
  "/releases/download/%s/pump-controller-esp32.ino.bin";

// ── HA discovery prefix ───────────────────────────────────
const char* HA_DISCOVERY_PREFIX = "homeassistant";

// ═══════════════════════════════════════════════════════════
//  RUNTIME CONFIG — loaded from NVS
// ═══════════════════════════════════════════════════════════

Preferences prefs;

struct Config {
  int     unitNumber;                   // 1-254
  char    wifiSSID[64];
  char    wifiPassword[64];
  bool    staticIP;
  uint8_t ip[4];
  uint8_t gw[4];
  uint8_t sn[4];
  uint8_t dns[4];
  char    mqttBroker[64];
  int     mqttPort;
  char    mqttUser[32];
  char    mqttPassword[64];
  char    syslogHost[64];
  int     syslogPort;
  int     pumpCount;                    // 1-MAX_PUMPS, default 1
  int     pumpPin[MAX_PUMPS];           // GPIO pin for each pump
  int     pumpDuration[MAX_PUMPS];      // seconds to run on "water" command
  int     waterLevelPin;                // GPIO for NO reed switch; -1 = disabled
  char    fwChannel[8];                 // "stable" (default) or "beta"
} cfg;

bool configLoaded = false;

// NVS magic — identifies config written by this firmware.
// Absent or wrong → clear namespace and return (configLoaded stays false → portal).
// Stricter than moisture sensor: no deployed devices, so any non-matching value
// (including absent) is treated as stale and cleared.
const char* NVS_MAGIC_KEY   = "magic";
const char* NVS_MAGIC_VALUE = "pump-ctrl-1";

// Forward-declare _logf so logf macro compiles before first use
#define logf(fmt, ...) _logf(__func__, fmt, ##__VA_ARGS__)

void loadConfig() {
  prefs.begin("pump", true);
  String magic = prefs.getString(NVS_MAGIC_KEY, "");
  prefs.end();
  if (magic != NVS_MAGIC_VALUE) {
    if (magic.length() > 0) {
      logf("Config    — NVS magic mismatch ('%s'), clearing\n", magic.c_str());
    } else {
      logf("Config    — NVS magic absent, clearing\n");
    }
    prefs.begin("pump", false);
    prefs.clear();
    prefs.end();
    return;  // configLoaded = false → portal
  }

  prefs.begin("pump", true);
  cfg.unitNumber  = prefs.getInt("unitNum", 0);
  prefs.getString("wifiSSID",   cfg.wifiSSID,    sizeof(cfg.wifiSSID));
  prefs.getString("wifiPass",   cfg.wifiPassword, sizeof(cfg.wifiPassword));
  cfg.staticIP    = prefs.getBool("staticIP", false);

  if (prefs.getBytes("ip",  cfg.ip,  4) == 0) {
    cfg.ip[0] = 192; cfg.ip[1] = 168; cfg.ip[2] = 211; cfg.ip[3] = cfg.unitNumber;
  }
  if (prefs.getBytes("gw",  cfg.gw,  4) == 0) {
    cfg.gw[0] = 192; cfg.gw[1] = 168; cfg.gw[2] =   1; cfg.gw[3] = 1;
  }
  if (prefs.getBytes("sn",  cfg.sn,  4) == 0) {
    cfg.sn[0] = 255; cfg.sn[1] = 255; cfg.sn[2] = 0; cfg.sn[3] = 0;
  }
  if (prefs.getBytes("dns", cfg.dns, 4) == 0) {
    cfg.dns[0] = 192; cfg.dns[1] = 168; cfg.dns[2] =   1; cfg.dns[3] = 1;
  }

  prefs.getString("mqttBroker", cfg.mqttBroker,  sizeof(cfg.mqttBroker));
  cfg.mqttPort    = prefs.getInt("mqttPort", 1883);
  prefs.getString("mqttUser",   cfg.mqttUser,    sizeof(cfg.mqttUser));
  prefs.getString("mqttPass",   cfg.mqttPassword, sizeof(cfg.mqttPassword));

  prefs.getString("syslogHost", cfg.syslogHost, sizeof(cfg.syslogHost));
  cfg.syslogPort  = prefs.getInt("syslogPort", 514);

  cfg.pumpCount   = prefs.getInt("pumpCount", 1);
  if (cfg.pumpCount < 1) cfg.pumpCount = 1;
  if (cfg.pumpCount > MAX_PUMPS) cfg.pumpCount = MAX_PUMPS;

  for (int i = 0; i < MAX_PUMPS; i++) {
    char keyPin[16], keyDur[16];
    snprintf(keyPin, sizeof(keyPin), "pumpPin%d", i);
    snprintf(keyDur, sizeof(keyDur), "pumpDur%d", i);
    cfg.pumpPin[i]      = prefs.getInt(keyPin, DEFAULT_PUMP_PINS[i]);
    cfg.pumpDuration[i] = prefs.getInt(keyDur, 5);
  }
  cfg.waterLevelPin = prefs.getInt("waterPin", -1);
  prefs.getString("fwChannel", cfg.fwChannel, sizeof(cfg.fwChannel));
  if (strlen(cfg.fwChannel) == 0) strlcpy(cfg.fwChannel, "stable", sizeof(cfg.fwChannel));

  prefs.end();

  configLoaded = (cfg.unitNumber > 0 &&
                  strlen(cfg.wifiSSID) > 0 &&
                  strlen(cfg.mqttBroker) > 0);
}

// Logs a warning for every pin conflict found in cfg.
// Called at boot after loadConfig() so problems surface in syslog immediately.
void validateConfig() {
  for (int i = 0; i < cfg.pumpCount; i++) {
    for (int j = i + 1; j < cfg.pumpCount; j++) {
      if (cfg.pumpPin[i] == cfg.pumpPin[j]) {
        logf("Config    — WARNING: pump %d and pump %d share GPIO%d\n",
          i + 1, j + 1, cfg.pumpPin[i]);
      }
    }
    if (cfg.waterLevelPin >= 0 && cfg.pumpPin[i] == cfg.waterLevelPin) {
      logf("Config    — WARNING: pump %d and water level sensor share GPIO%d\n",
        i + 1, cfg.waterLevelPin);
    }
  }
}

void clearConfig() {
  prefs.begin("pump", false);
  prefs.clear();
  prefs.end();
  logf("Config    — NVS cleared\n");
}

void saveConfig(const Config& c) {
  prefs.begin("pump", false);
  prefs.putString(NVS_MAGIC_KEY, NVS_MAGIC_VALUE);
  prefs.putInt("unitNum",        c.unitNumber);
  prefs.putString("wifiSSID",    c.wifiSSID);
  prefs.putString("wifiPass",    c.wifiPassword);
  prefs.putBool("staticIP",      c.staticIP);
  prefs.putBytes("ip",  c.ip,  4);
  prefs.putBytes("gw",  c.gw,  4);
  prefs.putBytes("sn",  c.sn,  4);
  prefs.putBytes("dns", c.dns, 4);
  prefs.putString("mqttBroker",  c.mqttBroker);
  prefs.putInt("mqttPort",       c.mqttPort);
  prefs.putString("mqttUser",    c.mqttUser);
  prefs.putString("mqttPass",    c.mqttPassword);
  prefs.putString("syslogHost",  c.syslogHost);
  prefs.putInt("syslogPort",     c.syslogPort);
  prefs.putInt("pumpCount",      c.pumpCount);
  for (int i = 0; i < MAX_PUMPS; i++) {
    char keyPin[16], keyDur[16];
    snprintf(keyPin, sizeof(keyPin), "pumpPin%d", i);
    snprintf(keyDur, sizeof(keyDur), "pumpDur%d", i);
    prefs.putInt(keyPin, c.pumpPin[i]);
    prefs.putInt(keyDur, c.pumpDuration[i]);
  }
  prefs.putInt("waterPin",       c.waterLevelPin);
  prefs.putString("fwChannel",  c.fwChannel);
  prefs.end();
  logf("Config    — saved to NVS\n");
}

// ═══════════════════════════════════════════════════════════
//  DERIVED TOPICS
// ═══════════════════════════════════════════════════════════

char UNIT_ID[16];            // e.g. "pump1"
char UNIT_NAME[32];          // e.g. "Garden Pump Controller 1"
char STATE_TOPIC[64];        // garden/pump1/state       (unit heartbeat)
char CMD_TOPIC[64];          // garden/pump1/cmd         (unit commands: restart/reset)
char PUMP_CMD_SUB[80];       // garden/pump1/pump/+/cmd  (wildcard subscription)
char CONFIG_SET_TOPIC[80];   // garden/pump1/config/set
char CONFIG_STATE_TOPIC[80]; // garden/pump1/config/state

// Per-pump topics built on demand (pump index 0-based)
void pumpStateTopic(int idx, char* buf, size_t len) {
  snprintf(buf, len, "garden/%s/pump/%d/state", UNIT_ID, idx + 1);
}
void pumpCmdTopic(int idx, char* buf, size_t len) {
  snprintf(buf, len, "garden/%s/pump/%d/cmd", UNIT_ID, idx + 1);
}

// HA discovery topics
char DISC_BTN_RESTART[128];
char DISC_BTN_RESET[128];
char DISC_FW[128];
// Per-pump discovery: built on demand
void pumpDiscTopic(int idx, char* buf, size_t len) {
  snprintf(buf, len, "%s/switch/%s_pump%d/config", HA_DISCOVERY_PREFIX, UNIT_ID, idx + 1);
}
// Config entity discovery topics
char DISC_CFG_MQTT_BROKER[128];
char DISC_CFG_MQTT_PORT[128];
char DISC_CFG_MQTT_USER[128];
char DISC_CFG_MQTT_PASS[128];
char DISC_CFG_SYSLOG_HOST[128];
char DISC_CFG_SYSLOG_PORT[128];
char DISC_CFG_PUMP_COUNT[128];
// Water level sensor
char WATER_LEVEL_TOPIC[64];
char DISC_WATER_LEVEL[128];
char DISC_CFG_WATER_PIN[128];
char DISC_CFG_FW_CHANNEL[128];
// Network config discovery topics
char DISC_CFG_STATIC_IP[128];
char DISC_CFG_IP[128];
char DISC_CFG_GW[128];
char DISC_CFG_SN[128];
char DISC_CFG_DNS[128];

void buildDerivedConfig() {
  snprintf(UNIT_ID,            sizeof(UNIT_ID),            "pump%d",                    cfg.unitNumber);
  snprintf(UNIT_NAME,          sizeof(UNIT_NAME),          "Garden Pump Controller %d", cfg.unitNumber);
  snprintf(STATE_TOPIC,        sizeof(STATE_TOPIC),        "garden/%s/state",           UNIT_ID);
  snprintf(CMD_TOPIC,          sizeof(CMD_TOPIC),          "garden/%s/cmd",             UNIT_ID);
  snprintf(PUMP_CMD_SUB,       sizeof(PUMP_CMD_SUB),       "garden/%s/pump/+/cmd",      UNIT_ID);
  snprintf(CONFIG_SET_TOPIC,   sizeof(CONFIG_SET_TOPIC),   "garden/%s/config/set",      UNIT_ID);
  snprintf(CONFIG_STATE_TOPIC, sizeof(CONFIG_STATE_TOPIC), "garden/%s/config/state",    UNIT_ID);

  snprintf(DISC_BTN_RESTART, sizeof(DISC_BTN_RESTART),
    "%s/button/%s_restart/config",         HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_BTN_RESET,   sizeof(DISC_BTN_RESET),
    "%s/button/%s_reset/config",           HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_FW,          sizeof(DISC_FW),
    "%s/sensor/%s_fw/config",              HA_DISCOVERY_PREFIX, UNIT_ID);

  snprintf(DISC_CFG_MQTT_BROKER,  sizeof(DISC_CFG_MQTT_BROKER),
    "%s/text/%s_cfg_mqtt_broker/config",   HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_MQTT_PORT,    sizeof(DISC_CFG_MQTT_PORT),
    "%s/number/%s_cfg_mqtt_port/config",   HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_MQTT_USER,    sizeof(DISC_CFG_MQTT_USER),
    "%s/text/%s_cfg_mqtt_user/config",     HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_MQTT_PASS,    sizeof(DISC_CFG_MQTT_PASS),
    "%s/text/%s_cfg_mqtt_pass/config",     HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_SYSLOG_HOST,  sizeof(DISC_CFG_SYSLOG_HOST),
    "%s/text/%s_cfg_syslog_host/config",   HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_SYSLOG_PORT,  sizeof(DISC_CFG_SYSLOG_PORT),
    "%s/number/%s_cfg_syslog_port/config", HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_PUMP_COUNT,   sizeof(DISC_CFG_PUMP_COUNT),
    "%s/number/%s_cfg_pump_count/config",  HA_DISCOVERY_PREFIX, UNIT_ID);

  snprintf(WATER_LEVEL_TOPIC,    sizeof(WATER_LEVEL_TOPIC),
    "garden/%s/water_level",               UNIT_ID);
  snprintf(DISC_WATER_LEVEL,     sizeof(DISC_WATER_LEVEL),
    "%s/binary_sensor/%s_water_level/config", HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_WATER_PIN,   sizeof(DISC_CFG_WATER_PIN),
    "%s/number/%s_cfg_water_pin/config",   HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_FW_CHANNEL,  sizeof(DISC_CFG_FW_CHANNEL),
    "%s/select/%s_cfg_fw_channel/config",  HA_DISCOVERY_PREFIX, UNIT_ID);

  snprintf(DISC_CFG_STATIC_IP,   sizeof(DISC_CFG_STATIC_IP),
    "%s/switch/%s_cfg_static_ip/config",   HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_IP,          sizeof(DISC_CFG_IP),
    "%s/text/%s_cfg_ip/config",            HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_GW,          sizeof(DISC_CFG_GW),
    "%s/text/%s_cfg_gw/config",            HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_SN,          sizeof(DISC_CFG_SN),
    "%s/text/%s_cfg_sn/config",            HA_DISCOVERY_PREFIX, UNIT_ID);
  snprintf(DISC_CFG_DNS,         sizeof(DISC_CFG_DNS),
    "%s/text/%s_cfg_dns/config",           HA_DISCOVERY_PREFIX, UNIT_ID);
}

// ═══════════════════════════════════════════════════════════
//  SYSLOG
//  Boot messages buffered until WiFi connects, then flushed.
// ═══════════════════════════════════════════════════════════

#define SYSLOG_LINES 24
#define SYSLOG_LINE  120
#define SYSLOG_FUNC  32

struct SyslogEntry {
  char msg[SYSLOG_LINE];
  char func[SYSLOG_FUNC];
};

static SyslogEntry syslogBuf[SYSLOG_LINES];
static int       syslogHead  = 0;
static int       syslogTotal = 0;
static bool      syslogReady = false;
static IPAddress syslogIP;

WiFiUDP syslogUdp;

void syslogSend(const char* func, const char* msg) {
  char clean[SYSLOG_LINE];
  strlcpy(clean, msg, sizeof(clean));
  int len = strlen(clean);
  while (len > 0 && (clean[len-1] == '\n' || clean[len-1] == '\r')) clean[--len] = '\0';
  if (len == 0) return;

  char timestamp[16];
  struct tm t;
  if (getLocalTime(&t)) {
    strftime(timestamp, sizeof(timestamp), "%b %e %T", &t);
  } else {
    strlcpy(timestamp, "Jan  1 00:00:00", sizeof(timestamp));
  }

  const char* hostname = (strlen(UNIT_ID) > 0) ? UNIT_ID : "pump";
  char packet[220];
  snprintf(packet, sizeof(packet), "<134>%s %s pump-controller-esp32[%s]: %s",
    timestamp, hostname, func, clean);

  syslogUdp.beginPacket(syslogIP, cfg.syslogPort);
  syslogUdp.print(packet);
  syslogUdp.endPacket();
}

void syslogFlush() {
  if (strlen(cfg.syslogHost) == 0) { syslogReady = true; return; }

  if (!syslogIP.fromString(cfg.syslogHost)) {
    if (WiFi.hostByName(cfg.syslogHost, syslogIP) != 1) {
      syslogReady = true;
      syslogHead  = 0;
      syslogTotal = 0;
      logf("DNS failed for '%s' — syslog disabled this session\n", cfg.syslogHost);
      return;
    }
  }

  // Discard buffered boot messages rather than sending them.
  //
  // WiFiUDP::endPacket() performs a synchronous ARP resolution when the ARP
  // cache is cold (first few seconds after WiFi association).  Each blocked
  // send takes ~4 s, so 5 buffered messages × 4 s ≈ 20 s of dead time at
  // boot.  The buffered lines are already visible on serial; syslog picks up
  // real-time messages from this point forward.
  syslogHead  = 0;
  syslogTotal = 0;
  syslogReady = true;
}

void _logf(const char* func, const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Serial.printf("[%s] %s", func, line);

  if (strlen(cfg.syslogHost) == 0) return;
  if (syslogReady && (uint32_t)syslogIP == 0) return;

  char sysline[SYSLOG_LINE];
  strlcpy(sysline, line, sizeof(sysline));

  if (syslogReady) {
    syslogSend(func, sysline);
  } else {
    strlcpy(syslogBuf[syslogHead].msg,  sysline, SYSLOG_LINE);
    strlcpy(syslogBuf[syslogHead].func, func,    SYSLOG_FUNC);
    syslogHead = (syslogHead + 1) % SYSLOG_LINES;
    syslogTotal++;
  }
}

// ═══════════════════════════════════════════════════════════
//  CAPTIVE PORTAL HTML
// ═══════════════════════════════════════════════════════════

WebServer server(80);
DNSServer dnsServer;

const char CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pump Controller Setup</title>
<style>
  body{font-family:sans-serif;max-width:460px;margin:40px auto;padding:0 16px;background:#f5f5f5}
  h1{font-size:1.3em;color:#1a5fa8;margin-bottom:4px}
  p.sub{color:#666;font-size:.85em;margin-top:0}
  label{display:block;margin-top:14px;font-size:.9em;color:#333;font-weight:600}
  input{width:100%;padding:8px;margin-top:4px;border:1px solid #ccc;
    border-radius:6px;font-size:1em;box-sizing:border-box}
  .optional{color:#888;font-weight:400;font-size:.8em}
  .section{background:#fff;border-radius:10px;padding:16px;margin:16px 0;
    box-shadow:0 1px 4px rgba(0,0,0,.08)}
  button[type=submit]{width:100%;padding:12px;background:#1a5fa8;color:#fff;border:none;
    border-radius:8px;font-size:1em;cursor:pointer;margin-top:20px}
  button[type=submit]:hover{background:#134a85}
  .hint{font-size:.78em;color:#888;margin-top:2px}
  .ip-wrap{display:flex;gap:4px;margin-top:4px;align-items:center}
  .ip-wrap input{width:58px;padding:8px;text-align:center;flex:none}
  .ip-wrap span{color:#555;font-weight:700}
  #network-rows{display:none;margin-top:4px}
  .chk-row{display:flex;align-items:center;gap:8px;margin-top:14px}
  .chk-row input{width:auto;margin:0}
  h3{font-size:.95em;color:#555;margin:16px 0 4px}
  details summary{cursor:pointer;font-weight:600;color:#555;font-size:.9em;margin-top:8px}
  .pw-wrap{position:relative;margin-top:4px}
  .pw-wrap input{margin-top:0;padding-right:38px}
  .pw-toggle{position:absolute;right:6px;top:50%;transform:translateY(-50%);
    width:auto;padding:4px;background:none;border:none;margin:0;
    color:#888;cursor:pointer;display:flex;align-items:center;line-height:1}
  .pw-toggle:hover{background:none;color:#333}
  .pump-row{display:flex;gap:8px;align-items:center;margin-top:10px}
  .pump-row label{margin:0;flex:0 0 60px}
  .pump-row input{flex:1}
  .pump-row .dur{flex:0 0 70px}
</style>
</head>
<body>
<h1>Pump Controller Setup</h1>
<p class="sub">Configure this controller then click Save. It will restart and begin listening for commands.</p>

<form method="POST" action="/save">

  <div class="section">
    <label>Unit number
      <input type="number" name="unitNum" id="unitNum" min="1" max="254"
        value="1" required oninput="syncNet()">
    </label>
    <p class="hint">Sets unit ID, friendly name, and default last IP octet</p>
  </div>

  <div class="section">
    <label>WiFi SSID
      <input type="text" name="ssid" placeholder="Your network name" required>
    </label>
    <label>WiFi password <span class="optional">(optional)</span>
      <div class="pw-wrap">
        <input type="password" name="wifiPass" placeholder="Leave blank for open networks">
        <button type="button" class="pw-toggle" onclick="togglePw(this)" aria-label="Show password">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor"
            stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
            <circle cx="12" cy="12" r="3"/>
          </svg>
        </button>
      </div>
    </label>
    <div class="chk-row">
      <input type="checkbox" name="staticIP" id="staticChk" onchange="toggleNet(this)">
      <label for="staticChk" style="margin:0;font-weight:600">Use static IP</label>
    </div>
    <div id="network-rows">
      <h3>IP address</h3>
      <div class="ip-wrap">
        <input type="number" name="ip1" id="ip1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip2" id="ip2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip3" id="ip3" value="211" min="0" max="255">
        <span>.</span>
        <input type="number" name="ip4" id="ip4" min="0" max="255">
      </div>
      <h3>Gateway</h3>
      <div class="ip-wrap">
        <input type="number" name="gw1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="gw4" value="1" min="0" max="255">
      </div>
      <h3>Subnet mask</h3>
      <div class="ip-wrap">
        <input type="number" name="sn1" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn2" value="255" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn3" value="0" min="0" max="255">
        <span>.</span>
        <input type="number" name="sn4" value="0" min="0" max="255">
      </div>
      <h3>DNS</h3>
      <div class="ip-wrap">
        <input type="number" name="dns1" value="192" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns2" value="168" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns3" value="1" min="0" max="255">
        <span>.</span>
        <input type="number" name="dns4" value="1" min="0" max="255">
      </div>
    </div>
  </div>

  <div class="section">
    <label>MQTT broker
      <input type="text" name="mqttBroker" placeholder="192.168.1.x or hostname" required>
    </label>
    <label>MQTT port
      <input type="number" name="mqttPort" value="1883" min="1" max="65535" required>
    </label>
    <label>MQTT username <span class="optional">(optional)</span>
      <input type="text" name="mqttUser" placeholder="Leave blank if not required">
    </label>
    <label>MQTT password <span class="optional">(optional)</span>
      <div class="pw-wrap">
        <input type="password" name="mqttPass" placeholder="Leave blank if not required">
        <button type="button" class="pw-toggle" onclick="togglePw(this)" aria-label="Show password">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor"
            stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
            <circle cx="12" cy="12" r="3"/>
          </svg>
        </button>
      </div>
    </label>
  </div>

  <div class="section">
    <details>
      <summary>Syslog <span class="optional">(optional)</span></summary>
      <label>Syslog host
        <input type="text" name="syslogHost" placeholder="hostname or IP">
      </label>
      <label>Syslog port
        <input type="number" name="syslogPort" value="514" min="1" max="65535">
      </label>
    </details>

    <details>
      <summary>Pump configuration</summary>
      <label style="margin-top:12px">Number of pumps
        <input type="number" name="pumpCount" id="pumpCount" value="1" min="1" max="5"
          oninput="updatePumpRows()">
      </label>
      <p class="hint">1–5. Only active pumps are controlled; inactive GPIO pins are not driven.</p>
      <div id="pump-rows">
        <!-- Pump rows injected by JS -->
      </div>
    </details>
  </div>

  <div class="section">
    <details>
      <summary>Water level sensor <span class="optional">(optional)</span></summary>
      <label style="margin-top:12px">Reed switch GPIO pin
        <input type="number" name="waterPin" value="-1" min="-1" max="28">
      </label>
      <p class="hint">Normally-open reed switch with float and magnet. Float drops when tank is low, bringing the magnet to the reed and closing it (GPIO LOW = water low). Reed open (GPIO HIGH) = water OK. Set to -1 to disable.</p>
    </details>
  </div>

  <button type="submit">Save &amp; Restart</button>
</form>

<script>
function syncNet() {
  var n = parseInt(document.getElementById('unitNum').value)||1;
  document.getElementById('ip4').value = n;
}
function toggleNet(chk) {
  document.getElementById('network-rows').style.display = chk.checked ? 'block' : 'none';
}
function togglePw(btn) {
  var inp = btn.parentElement.querySelector('input');
  inp.type = inp.type === 'password' ? 'text' : 'password';
}
function updatePumpRows() {
  var raw = parseInt(document.getElementById('pumpCount').value) || 1;
  var count = Math.min(Math.max(raw, 1), 5);
  document.getElementById('pumpCount').value = count;  // clamp visible value too
  var container = document.getElementById('pump-rows');
  container.innerHTML = '';
  for (var i = 0; i < count; i++) {
    var n = i + 1;
    var div = document.createElement('div');
    div.innerHTML =
      '<label style="margin-top:12px;font-size:.85em">Pump ' + n + '</label>' +
      '<div class="pump-row">' +
        '<label style="flex:0 0 auto;margin:0;font-size:.85em">GPIO pin</label>' +
        '<input type="number" name="pumpPin' + i + '" value="' + n + '" min="0" max="28">' +
        '<label style="flex:0 0 auto;margin:0;font-size:.85em">Duration (s)</label>' +
        '<input type="number" name="pumpDur' + i + '" value="5" min="1" max="30">' +
      '</div>';
    container.appendChild(div);
  }
}
// Initialise on load
window.addEventListener('DOMContentLoaded', function() {
  syncNet();
  updatePumpRows();
});
</script>
</body>
</html>
)rawhtml";

// ── Portal: input validation helpers ─────────────────────

bool isValidOctet(const String& s) {
  if (s.length() == 0 || s.length() > 3) return false;
  for (char c : s) if (!isDigit(c)) return false;
  return s.toInt() >= 0 && s.toInt() <= 255;
}
bool isValidPort(int p) { return p >= 1 && p <= 65535; }

// ── Portal: GET / ─────────────────────────────────────────

void handleRoot() {
  server.send_P(200, "text/html", CONFIG_HTML);
}

// ── Portal: POST /save ────────────────────────────────────

void handleSave() {
  // Unit number
  if (!server.hasArg("unitNum") || server.arg("unitNum").toInt() < 1 ||
      server.arg("unitNum").toInt() > 254) {
    server.send(400, "text/plain", "Unit number must be 1-254"); return;
  }
  // SSID
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.send(400, "text/plain", "WiFi SSID required"); return;
  }
  // MQTT broker
  if (!server.hasArg("mqttBroker") || server.arg("mqttBroker").length() == 0) {
    server.send(400, "text/plain", "MQTT broker required"); return;
  }
  // MQTT port
  if (!isValidPort(server.arg("mqttPort").toInt())) {
    server.send(400, "text/plain", "MQTT port must be 1-65535"); return;
  }
  // Pump count
  int pcount = server.arg("pumpCount").toInt();
  if (pcount < 1 || pcount > MAX_PUMPS) {
    server.send(400, "text/plain", "Pump count must be 1-5"); return;
  }

  Config c = {};
  c.unitNumber = server.arg("unitNum").toInt();
  strlcpy(c.wifiSSID,    server.arg("ssid").c_str(),       sizeof(c.wifiSSID));
  strlcpy(c.wifiPassword, server.arg("wifiPass").c_str(),  sizeof(c.wifiPassword));
  c.staticIP = server.hasArg("staticIP") && server.arg("staticIP") == "on";

  if (c.staticIP) {
    c.ip[0] = server.arg("ip1").toInt();  c.ip[1] = server.arg("ip2").toInt();
    c.ip[2] = server.arg("ip3").toInt();  c.ip[3] = server.arg("ip4").toInt();
    c.gw[0] = server.arg("gw1").toInt();  c.gw[1] = server.arg("gw2").toInt();
    c.gw[2] = server.arg("gw3").toInt();  c.gw[3] = server.arg("gw4").toInt();
    c.sn[0] = server.arg("sn1").toInt();  c.sn[1] = server.arg("sn2").toInt();
    c.sn[2] = server.arg("sn3").toInt();  c.sn[3] = server.arg("sn4").toInt();
    c.dns[0] = server.arg("dns1").toInt(); c.dns[1] = server.arg("dns2").toInt();
    c.dns[2] = server.arg("dns3").toInt(); c.dns[3] = server.arg("dns4").toInt();
  }

  strlcpy(c.mqttBroker,   server.arg("mqttBroker").c_str(), sizeof(c.mqttBroker));
  c.mqttPort = server.arg("mqttPort").toInt();
  strlcpy(c.mqttUser,     server.arg("mqttUser").c_str(),   sizeof(c.mqttUser));
  strlcpy(c.mqttPassword, server.arg("mqttPass").c_str(),   sizeof(c.mqttPassword));
  strlcpy(c.syslogHost,   server.arg("syslogHost").c_str(), sizeof(c.syslogHost));
  c.syslogPort = server.arg("syslogPort").length() ? server.arg("syslogPort").toInt() : 514;

  c.pumpCount = pcount;
  for (int i = 0; i < MAX_PUMPS; i++) {
    char keyPin[16], keyDur[16];
    snprintf(keyPin, sizeof(keyPin), "pumpPin%d", i);
    snprintf(keyDur, sizeof(keyDur), "pumpDur%d", i);
    int pin = server.hasArg(keyPin) ? server.arg(keyPin).toInt() : DEFAULT_PUMP_PINS[i];
    int dur = server.hasArg(keyDur) ? server.arg(keyDur).toInt() : 5;
    c.pumpPin[i]      = (pin >= 0 && pin <= 28) ? pin : DEFAULT_PUMP_PINS[i];
    c.pumpDuration[i] = (dur >= 1 && dur <= PUMP_MAX_DURATION_S) ? dur : 5;
  }

  int waterPin = server.hasArg("waterPin") ? server.arg("waterPin").toInt() : -1;
  c.waterLevelPin = (waterPin >= -1 && waterPin <= 28) ? waterPin : -1;

  // Reject conflicting GPIO assignments
  for (int i = 0; i < c.pumpCount; i++) {
    for (int j = i + 1; j < c.pumpCount; j++) {
      if (c.pumpPin[i] == c.pumpPin[j]) {
        server.send(400, "text/plain",
          "Pump " + String(i+1) + " and pump " + String(j+1) + " share the same GPIO"); return;
      }
    }
    if (c.waterLevelPin >= 0 && c.pumpPin[i] == c.waterLevelPin) {
      server.send(400, "text/plain",
        "Pump " + String(i+1) + " and water level sensor share the same GPIO"); return;
    }
  }

  saveConfig(c);
  server.send(200, "text/html",
    "<html><body><h2 style='font-family:sans-serif;color:#1a5fa8'>Saved! Restarting...</h2></body></html>");
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + server.client().localIP().toString(), true);
  server.send(302, "text/plain", "");
}

// ═══════════════════════════════════════════════════════════
//  WIFI — connect or start AP portal
// ═══════════════════════════════════════════════════════════

bool connectWiFi() {
  logf("WiFi      — connecting to '%s'\n", cfg.wifiSSID);
  if (cfg.staticIP) {
    WiFi.config(
      IPAddress(cfg.ip[0],  cfg.ip[1],  cfg.ip[2],  cfg.ip[3]),
      IPAddress(cfg.gw[0],  cfg.gw[1],  cfg.gw[2],  cfg.gw[3]),
      IPAddress(cfg.sn[0],  cfg.sn[1],  cfg.sn[2],  cfg.sn[3]),
      IPAddress(cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3])
    );
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    logf("WiFi      — connected, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  logf("WiFi      — connection failed\n");
  return false;
}

// Start captive portal AP and serve config page until timeout or save
void startPortal() {
  logf("Portal    — starting AP\n");
  WiFi.mode(WIFI_AP);

  char apSSID[32];
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(apSSID, sizeof(apSSID), "PumpController-%02X%02X", mac[4], mac[5]);

  WiFi.softAP(apSSID, AP_PASSWORD);
  logf("Portal    — AP '%s', IP %s\n", apSSID, WiFi.softAPIP().toString().c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/save",   HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long deadline = millis() + (unsigned long)AP_TIMEOUT_MIN * 60 * 1000UL;
  while (millis() < deadline) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
  }

  logf("Portal    — timed out, sleeping\n");
  delay(500);
  // Deep sleep not available on always-awake controller; just restart
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════
//  PUMP STATE MACHINE
// ═══════════════════════════════════════════════════════════

struct PumpSlot {
  bool          running;
  unsigned long startMs;
  int           durationMs;
};

static PumpSlot pumpSlot[MAX_PUMPS];

// Forward declaration — defined with updateWaterLevel() below startPump().
static bool waterLevelLow = false;

void stopPump(int idx) {
  if (idx < 0 || idx >= MAX_PUMPS) return;
  bool wasRunning = pumpSlot[idx].running;
  pumpSlot[idx].running = false;
  if (idx < cfg.pumpCount) {
    digitalWrite(cfg.pumpPin[idx], LOW);
  }
  if (wasRunning) {
    logf("Pump %d   — stopped (GPIO%d LOW)\n", idx + 1, cfg.pumpPin[idx]);
  } else {
    logf("Pump %d   — stop requested but was already idle\n", idx + 1);
  }
}

void startPump(int idx) {
  if (idx < 0 || idx >= cfg.pumpCount) return;
  if (cfg.waterLevelPin >= 0 && waterLevelLow) {
    logf("Pump %d   — blocked (water LOW)\n", idx + 1);
    return;
  }
  int capDur = min(cfg.pumpDuration[idx], PUMP_MAX_DURATION_S);
  pumpSlot[idx].running    = true;
  pumpSlot[idx].startMs    = millis();
  pumpSlot[idx].durationMs = capDur * 1000;
  digitalWrite(cfg.pumpPin[idx], HIGH);
  logf("Pump %d   — started for %ds (GPIO%d)\n", idx + 1, capDur, cfg.pumpPin[idx]);
}

void initPumpPins() {
  for (int i = 0; i < cfg.pumpCount; i++) {
    pinMode(cfg.pumpPin[i], OUTPUT);
    digitalWrite(cfg.pumpPin[i], LOW);
    pumpSlot[i].running = false;
  }
}

// ═══════════════════════════════════════════════════════════
//  MQTT
// ═══════════════════════════════════════════════════════════

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ── Deferred command flags (set in callback, processed in loop) ──
struct PendingPumpCmd {
  bool active;
  int  idx;    // 0-based pump index
  bool water;  // true=water, false=stop
};
static PendingPumpCmd pendingPumpCmd = { false, 0, false };
static bool pendingRestart = false;
static bool pendingReset   = false;

// Config update payload (JSON) from MQTT
static char  pendingConfigPayload[256];
static bool  pendingConfigUpdate = false;
static bool  pendingFotaCheck   = false;

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  int len = min((int)length, (int)sizeof(msg) - 1);
  memcpy(msg, payload, len);
  msg[len] = '\0';

  // Unit-level command: garden/pumpN/cmd
  if (strcmp(topic, CMD_TOPIC) == 0) {
    if (strcmp(msg, "restart") == 0) pendingRestart = true;
    if (strcmp(msg, "reset")   == 0) pendingReset   = true;
    return;
  }

  // Config update: garden/pumpN/config/set
  if (strcmp(topic, CONFIG_SET_TOPIC) == 0) {
    strlcpy(pendingConfigPayload, msg, sizeof(pendingConfigPayload));
    pendingConfigUpdate = true;
    return;
  }

  // Per-pump command: garden/pumpN/pump/{I}/cmd  (wildcard matched on subscription)
  // Extract pump index I from topic: "garden/pumpN/pump/I/cmd"
  char prefix[80];
  snprintf(prefix, sizeof(prefix), "garden/%s/pump/", UNIT_ID);
  size_t prefixLen = strlen(prefix);
  if (strncmp(topic, prefix, prefixLen) == 0) {
    int pumpNumber = atoi(topic + prefixLen);   // 1-based
    int idx = pumpNumber - 1;                    // 0-based
    if (idx >= 0 && idx < cfg.pumpCount) {
      // Serial only — no logf/syslog inside MQTT callback
      Serial.printf("[mqttCallback] pump %d cmd: '%s'\n", idx + 1, msg);
      pendingPumpCmd.active = true;
      pendingPumpCmd.idx    = idx;
      pendingPumpCmd.water  = (strcmp(msg, "water") == 0);
    }
  }
}

void publishPumpState(int idx) {
  char topic[80];
  pumpStateTopic(idx, topic, sizeof(topic));
  const char* state = pumpSlot[idx].running ? "running" : "idle";
  mqtt.publish(topic, state, true);  // retained so HA picks it up on subscribe
}

// ── Water level sensor state ──────────────────────────────
// NO reed switch: float drops when tank is low → magnet reaches reed → reed closes → GPIO LOW = water LOW.
// Float up (water OK) → magnet away from reed → reed open → INPUT_PULLUP holds GPIO HIGH = OK.
// waterLevelLow declared above startPump() so the pump guard can reference it.
static int  waterLevelPinLast = -2;     // last raw read (-2 = uninitialised)
static unsigned long waterLevelStableMs = 0;

void publishWaterLevel() {
  if (cfg.waterLevelPin < 0) return;
  mqtt.publish(WATER_LEVEL_TOPIC, waterLevelLow ? "LOW" : "OK", true);
}

void updateWaterLevel() {
  if (cfg.waterLevelPin < 0) return;
  int raw = digitalRead(cfg.waterLevelPin);   // LOW = reed closed = water low
  if (raw != waterLevelPinLast) {
    waterLevelPinLast = raw;
    waterLevelStableMs = millis();
    return;  // restart debounce timer
  }
  if (millis() - waterLevelStableMs < WATER_DEBOUNCE_MS) return;  // still settling
  bool nowLow = (raw == LOW);
  if (nowLow == waterLevelLow) return;  // no change
  waterLevelLow = nowLow;
  if (waterLevelLow) {
    logf("Water     — LOW (reed closed, GPIO%d LOW)\n", cfg.waterLevelPin);
    for (int i = 0; i < cfg.pumpCount; i++) stopPump(i);
  } else {
    logf("Water     — OK (reed open, GPIO%d HIGH)\n", cfg.waterLevelPin);
  }
  if (mqtt.connected()) publishWaterLevel();
}

void publishUnitState() {
  unsigned long uptimeSec = millis() / 1000;
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"firmware_version\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d,\"pump_count\":%d}",
    FIRMWARE_VERSION, uptimeSec, WiFi.RSSI(), cfg.pumpCount);
  mqtt.publish(STATE_TOPIC, payload, false);
}

void publishConfigState() {
  // Build per-pump pin/duration arrays as JSON
  char pins[80] = "[";
  char durs[80] = "[";
  for (int i = 0; i < MAX_PUMPS; i++) {
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "%d%s", cfg.pumpPin[i],      i < MAX_PUMPS-1 ? "," : "]");
    strlcat(pins, tmp, sizeof(pins));
    snprintf(tmp, sizeof(tmp), "%d%s", cfg.pumpDuration[i], i < MAX_PUMPS-1 ? "," : "]");
    strlcat(durs, tmp, sizeof(durs));
  }
  char ipStr[16], gwStr[16], snStr[16], dnsStr[16];
  snprintf(ipStr,  sizeof(ipStr),  "%d.%d.%d.%d", cfg.ip[0],  cfg.ip[1],  cfg.ip[2],  cfg.ip[3]);
  snprintf(gwStr,  sizeof(gwStr),  "%d.%d.%d.%d", cfg.gw[0],  cfg.gw[1],  cfg.gw[2],  cfg.gw[3]);
  snprintf(snStr,  sizeof(snStr),  "%d.%d.%d.%d", cfg.sn[0],  cfg.sn[1],  cfg.sn[2],  cfg.sn[3]);
  snprintf(dnsStr, sizeof(dnsStr), "%d.%d.%d.%d", cfg.dns[0], cfg.dns[1], cfg.dns[2], cfg.dns[3]);

  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"mqttBroker\":\"%s\",\"mqttPort\":%d,\"mqttUser\":\"%s\","
    "\"mqttPassword\":\"***\","
    "\"syslogHost\":\"%s\",\"syslogPort\":%d,"
    "\"pumpCount\":%d,\"pumpPins\":%s,\"pumpDurations\":%s,"
    "\"staticIP\":%s,\"ip\":\"%s\",\"gw\":\"%s\",\"sn\":\"%s\",\"dns\":\"%s\","
    "\"waterLevelPin\":%d,\"fwChannel\":\"%s\"}",
    cfg.mqttBroker, cfg.mqttPort, cfg.mqttUser,
    cfg.syslogHost, cfg.syslogPort,
    cfg.pumpCount, pins, durs,
    cfg.staticIP ? "true" : "false", ipStr, gwStr, snStr, dnsStr,
    cfg.waterLevelPin, cfg.fwChannel);
  mqtt.publish(CONFIG_STATE_TOPIC, payload, true);
}

void applyConfigUpdate(const char* json) {
  // Lightweight JSON field extraction — no heap allocation, no library dependency.
  // Only processes keys we care about; malformed JSON is silently ignored.
  auto extractStr = [](const char* j, const char* key, char* out, size_t outLen) -> bool {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(j, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    size_t len = min((size_t)(end - p), outLen - 1);
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
  };
  auto extractInt = [](const char* j, const char* key, int* out) -> bool {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* p = strstr(j, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ') p++;
    if (!isDigit(*p) && *p != '-') return false;
    *out = atoi(p);
    return true;
  };

  // Helper: parse a dotted IPv4 string into a 4-byte array
  auto parseIP = [](const char* str, uint8_t* bytes) -> bool {
    IPAddress addr;
    if (!addr.fromString(str)) return false;
    bytes[0] = addr[0]; bytes[1] = addr[1]; bytes[2] = addr[2]; bytes[3] = addr[3];
    return true;
  };

  char tmp[64];
  int  ival;

  if (extractStr(json, "mqttBroker",   tmp, sizeof(tmp))) strlcpy(cfg.mqttBroker,   tmp, sizeof(cfg.mqttBroker));
  if (extractInt(json, "mqttPort",     &ival) && isValidPort(ival)) cfg.mqttPort = ival;
  if (extractStr(json, "mqttUser",     tmp, sizeof(tmp))) strlcpy(cfg.mqttUser,     tmp, sizeof(cfg.mqttUser));
  if (extractStr(json, "mqttPassword", tmp, sizeof(tmp))) strlcpy(cfg.mqttPassword, tmp, sizeof(cfg.mqttPassword));
  if (extractStr(json, "syslogHost",   tmp, sizeof(tmp))) strlcpy(cfg.syslogHost,   tmp, sizeof(cfg.syslogHost));
  if (extractInt(json, "syslogPort",   &ival) && isValidPort(ival)) cfg.syslogPort = ival;
  if (extractInt(json, "pumpCount",    &ival) && ival >= 1 && ival <= MAX_PUMPS) cfg.pumpCount = ival;

  // Network / static IP
  if (strstr(json, "\"staticIP\":true"))  cfg.staticIP = true;
  if (strstr(json, "\"staticIP\":false")) cfg.staticIP = false;
  if (extractStr(json, "ip",  tmp, sizeof(tmp))) parseIP(tmp, cfg.ip);
  if (extractStr(json, "gw",  tmp, sizeof(tmp))) parseIP(tmp, cfg.gw);
  if (extractStr(json, "sn",  tmp, sizeof(tmp))) parseIP(tmp, cfg.sn);
  if (extractStr(json, "dns", tmp, sizeof(tmp))) parseIP(tmp, cfg.dns);
  if (extractStr(json, "fwChannel", tmp, sizeof(tmp))) {
    if (strcmp(tmp, "stable") == 0 || strcmp(tmp, "beta") == 0) {
      if (strcmp(tmp, cfg.fwChannel) != 0) {
        strlcpy(cfg.fwChannel, tmp, sizeof(cfg.fwChannel));
        logf("Config    — fwChannel -> %s, scheduling FOTA check\n", tmp);
        pendingFotaCheck = true;
      }
    } else {
      logf("Config    — fwChannel rejected: must be 'stable' or 'beta'\n");
    }
  }
  if (extractInt(json, "waterLevelPin", &ival) && ival >= -1 && ival <= 28) {
    bool conflict = false;
    // Only check against active pump slots — inactive slots hold stale defaults
    for (int i = 0; i < cfg.pumpCount && !conflict; i++)
      if (ival >= 0 && cfg.pumpPin[i] == ival) conflict = true;
    if (!conflict) cfg.waterLevelPin = ival;
    else logf("Config    — waterLevelPin %d rejected (conflicts with pump pin)\n", ival);
  }

  for (int i = 0; i < MAX_PUMPS; i++) {
    char keyPin[24], keyDur[24];
    snprintf(keyPin, sizeof(keyPin), "pumpPin%d", i);
    snprintf(keyDur, sizeof(keyDur), "pumpDuration%d", i);
    if (extractInt(json, keyPin, &ival) && ival >= 0 && ival <= 28) {
      bool conflict = (ival == cfg.waterLevelPin);
      // Only check against active pump slots — inactive slots hold stale defaults
      for (int j = 0; j < cfg.pumpCount && !conflict; j++)
        if (j != i && cfg.pumpPin[j] == ival) conflict = true;
      if (!conflict) cfg.pumpPin[i] = ival;
      else logf("Config    — pumpPin%d=%d rejected (conflicts with existing pin)\n", i, ival);
    }
    if (extractInt(json, keyDur, &ival) && ival >= 1 && ival <= PUMP_MAX_DURATION_S)
      cfg.pumpDuration[i] = ival;
  }

  saveConfig(cfg);
  publishConfigState();
  logf("Config    — remote update applied\n");
}

// ── HA autodiscovery ──────────────────────────────────────

void publishHADiscovery() {
  char dev[200];
  char mac[20];
  uint8_t rawMac[6];
  esp_read_mac(rawMac, ESP_MAC_WIFI_STA);
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
    rawMac[0], rawMac[1], rawMac[2], rawMac[3], rawMac[4], rawMac[5]);
  snprintf(dev, sizeof(dev),
    "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
    "\"manufacturer\":\"Craig\",\"model\":\"ESP32-C6 Pump Controller\","
    "\"sw_version\":\"%s\"}",
    UNIT_ID, UNIT_NAME, FIRMWARE_VERSION);

  char payload[768];

  // ── Firmware version sensor ───────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Firmware Version\",\"unique_id\":\"%s_fw\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.firmware_version}}\","
    "\"entity_category\":\"diagnostic\",%s}",
    UNIT_ID, STATE_TOPIC, dev);
  mqtt.publish(DISC_FW, payload, true);

  // ── Restart button ────────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Restart\",\"unique_id\":\"%s_restart\","
    "\"command_topic\":\"%s\",\"payload_press\":\"restart\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CMD_TOPIC, dev);
  mqtt.publish(DISC_BTN_RESTART, payload, true);

  // ── Reset Config button ───────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Reset Config\",\"unique_id\":\"%s_reset\","
    "\"command_topic\":\"%s\",\"payload_press\":\"reset\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CMD_TOPIC, dev);
  mqtt.publish(DISC_BTN_RESET, payload, true);

  // ── Per-pump switch entities ──────────────────────────────
  for (int i = 0; i < cfg.pumpCount; i++) {
    char discTopic[128], statTopic[80], cmdTopic[80];
    pumpDiscTopic(i,    discTopic, sizeof(discTopic));
    pumpStateTopic(i,  statTopic, sizeof(statTopic));
    pumpCmdTopic(i,    cmdTopic,  sizeof(cmdTopic));

    char pumpName[32];
    snprintf(pumpName, sizeof(pumpName), "Pump %d", i + 1);

    snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"unique_id\":\"%s_pump%d\","
      "\"state_topic\":\"%s\","
      "\"command_topic\":\"%s\","
      "\"payload_on\":\"water\",\"payload_off\":\"stop\","
      "\"state_on\":\"running\",\"state_off\":\"idle\","
      "\"device_class\":\"switch\","
      "\"icon\":\"mdi:water-pump\",%s}",
      pumpName, UNIT_ID, i + 1,
      statTopic, cmdTopic,
      dev);
    mqtt.publish(discTopic, payload, true);
  }

  // ── Config: MQTT broker ───────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"MQTT Broker\",\"unique_id\":\"%s_cfg_mqtt_broker\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.mqttBroker}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"mqttBroker\\\":\\\"{{value}}\\\"}\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_MQTT_BROKER, payload, true);

  // ── Config: MQTT port ─────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"MQTT Port\",\"unique_id\":\"%s_cfg_mqtt_port\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.mqttPort}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"mqttPort\\\":{{value}}}\","
    "\"min\":1,\"max\":65535,\"mode\":\"box\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_MQTT_PORT, payload, true);

  // ── Config: MQTT user ─────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"MQTT User\",\"unique_id\":\"%s_cfg_mqtt_user\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.mqttUser}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"mqttUser\\\":\\\"{{value}}\\\"}\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_MQTT_USER, payload, true);

  // ── Config: MQTT password ─────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"MQTT Password\",\"unique_id\":\"%s_cfg_mqtt_pass\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.mqttPassword}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"mqttPassword\\\":\\\"{{value}}\\\"}\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_MQTT_PASS, payload, true);

  // ── Config: syslog host ───────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Syslog Host\",\"unique_id\":\"%s_cfg_syslog_host\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.syslogHost}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"syslogHost\\\":\\\"{{value}}\\\"}\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_SYSLOG_HOST, payload, true);

  // ── Config: syslog port ───────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Syslog Port\",\"unique_id\":\"%s_cfg_syslog_port\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.syslogPort}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"syslogPort\\\":{{value}}}\","
    "\"min\":1,\"max\":65535,\"mode\":\"box\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_SYSLOG_PORT, payload, true);

  // ── Config: pump count ────────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Pump Count\",\"unique_id\":\"%s_cfg_pump_count\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.pumpCount}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"pumpCount\\\":{{value}}}\","
    "\"min\":1,\"max\":5,\"mode\":\"box\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_PUMP_COUNT, payload, true);

  // ── Config: per-pump GPIO pin and duration ────────────────
  for (int i = 0; i < cfg.pumpCount; i++) {
    char discPin[128], discDur[128];
    snprintf(discPin, sizeof(discPin),
      "%s/number/%s_cfg_pump%d_pin/config", HA_DISCOVERY_PREFIX, UNIT_ID, i + 1);
    snprintf(discDur, sizeof(discDur),
      "%s/number/%s_cfg_pump%d_dur/config", HA_DISCOVERY_PREFIX, UNIT_ID, i + 1);

    snprintf(payload, sizeof(payload),
      "{\"name\":\"Pump %d GPIO Pin\",\"unique_id\":\"%s_cfg_pump%d_pin\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{value_json.pumpPins[%d]}}\","
      "\"command_topic\":\"%s\",\"command_template\":\"{\\\"pumpPin%d\\\":{{value}}}\","
      "\"min\":0,\"max\":28,\"mode\":\"box\","
      "\"entity_category\":\"config\",%s}",
      i + 1, UNIT_ID, i + 1,
      CONFIG_STATE_TOPIC, i,
      CONFIG_SET_TOPIC, i,
      dev);
    mqtt.publish(discPin, payload, true);

    snprintf(payload, sizeof(payload),
      "{\"name\":\"Pump %d Duration (s)\",\"unique_id\":\"%s_cfg_pump%d_dur\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{value_json.pumpDurations[%d]}}\","
      "\"command_topic\":\"%s\",\"command_template\":\"{\\\"pumpDuration%d\\\":{{value}}}\","
      "\"min\":1,\"max\":%d,\"mode\":\"box\","
      "\"entity_category\":\"config\",%s}",
      i + 1, UNIT_ID, i + 1,
      CONFIG_STATE_TOPIC, i,
      CONFIG_SET_TOPIC, i,
      PUMP_MAX_DURATION_S,
      dev);
    mqtt.publish(discDur, payload, true);
  }

  // ── Water level sensor ────────────────────────────────────
  // Binary sensor — ON (problem) = water low, OFF = OK.
  // Published only when a pin is configured; cleared (empty retained) when disabled.
  if (cfg.waterLevelPin >= 0) {
    snprintf(payload, sizeof(payload),
      "{\"name\":\"Water Level\",\"unique_id\":\"%s_water_level\","
      "\"state_topic\":\"%s\","
      "\"payload_on\":\"LOW\",\"payload_off\":\"OK\","
      "\"device_class\":\"problem\","
      "\"icon\":\"mdi:water-alert\",%s}",
      UNIT_ID, WATER_LEVEL_TOPIC, dev);
  } else {
    payload[0] = '\0';  // empty payload un-publishes any previous retained entity
  }
  mqtt.publish(DISC_WATER_LEVEL, payload, true);

  // Config: water level pin (-1 = disabled, 0-28 = GPIO)
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Water Level Pin\",\"unique_id\":\"%s_cfg_water_pin\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.waterLevelPin}}\","
    "\"command_topic\":\"%s\",\"command_template\":\"{\\\"waterLevelPin\\\":{{value}}}\","
    "\"min\":-1,\"max\":28,\"mode\":\"box\","
    "\"icon\":\"mdi:electric-switch\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_WATER_PIN, payload, true);

  // ── Update channel select ────────────────────────────────
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Update Channel\",\"unique_id\":\"%s_cfg_fw_channel\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{{value_json.fwChannel}}\","
    "\"command_topic\":\"%s\","
    "\"command_template\":\"{\\\"fwChannel\\\":\\\"{{value}}\\\"}\","
    "\"options\":[\"stable\",\"beta\"],"
    "\"entity_category\":\"config\","
    "\"icon\":\"mdi:update\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_FW_CHANNEL, payload, true);

  // ── Network config ────────────────────────────────────────
  // Static IP switch — payload_on/off are raw JSON sent to config/set topic
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Static IP\",\"unique_id\":\"%s_cfg_static_ip\","
    "\"state_topic\":\"%s\","
    "\"value_template\":\"{%% if value_json.staticIP %%}ON{%% else %%}OFF{%% endif %%}\","
    "\"command_topic\":\"%s\","
    "\"payload_on\":\"{\\\"staticIP\\\":true}\","
    "\"payload_off\":\"{\\\"staticIP\\\":false}\","
    "\"entity_category\":\"config\",%s}",
    UNIT_ID, CONFIG_STATE_TOPIC, CONFIG_SET_TOPIC, dev);
  mqtt.publish(DISC_CFG_STATIC_IP, payload, true);

  // IP address, gateway, subnet mask, DNS — text entities
  const struct { const char* name; const char* uid; const char* key; const char* disc; } netFields[] = {
    { "IP Address",  "cfg_ip",  "ip",  DISC_CFG_IP  },
    { "Gateway",     "cfg_gw",  "gw",  DISC_CFG_GW  },
    { "Subnet Mask", "cfg_sn",  "sn",  DISC_CFG_SN  },
    { "DNS Server",  "cfg_dns", "dns", DISC_CFG_DNS },
  };
  for (auto& f : netFields) {
    snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
      "\"state_topic\":\"%s\","
      "\"value_template\":\"{{value_json.%s}}\","
      "\"command_topic\":\"%s\","
      "\"command_template\":\"{\\\"" "%s" "\\\":\\\"{{value}}\\\"}\","
      "\"entity_category\":\"config\",%s}",
      f.name, UNIT_ID, f.uid,
      CONFIG_STATE_TOPIC, f.key,
      CONFIG_SET_TOPIC, f.key,
      dev);
    mqtt.publish(f.disc, payload, true);
  }

  logf("MQTT      — HA discovery published\n");
}

// ── MQTT connect (called on boot and on reconnect) ────────

bool mqttConnect() {
  mqtt.setServer(cfg.mqttBroker, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(MQTT_TIMEOUT_S);

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "pump-ctrl-%d", cfg.unitNumber);
  logf("MQTT      — connecting to %s:%d as '%s'\n",
    cfg.mqttBroker, cfg.mqttPort, clientId);

  bool ok;
  if (strlen(cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId, cfg.mqttUser, cfg.mqttPassword);
  } else {
    ok = mqtt.connect(clientId);
  }

  if (!ok) {
    logf("MQTT      — connect failed, state=%d\n", mqtt.state());
    return false;
  }

  logf("MQTT      — connected\n");

  // Subscribe
  mqtt.subscribe(CMD_TOPIC);
  mqtt.subscribe(PUMP_CMD_SUB);
  mqtt.subscribe(CONFIG_SET_TOPIC);

  // Publish initial state
  publishHADiscovery();
  publishConfigState();
  publishUnitState();
  for (int i = 0; i < cfg.pumpCount; i++) publishPumpState(i);
  publishWaterLevel();

  return true;
}

// ═══════════════════════════════════════════════════════════
//  FOTA
// ═══════════════════════════════════════════════════════════

void checkForUpdate() {
  bool isBeta = strcmp(cfg.fwChannel, "beta") == 0;
  bool isDev  = strchr(FIRMWARE_VERSION, '-') != nullptr;

  // Stable channel: skip dev builds — no stable tag to compare against.
  // Beta channel always checks — dev builds may be promoted to a beta tag.
  if (!isBeta && isDev) {
    logf("FOTA      — skipped: dev build on stable channel (%s)\n", FIRMWARE_VERSION);
    return;
  }

  logf("FOTA      — checking for update (%s channel)...\n",
       isBeta ? "beta" : "stable");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(FOTA_VERSION_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(FOTA_VERSION_TIMEOUT_MS / 1000);

  HTTPClient http;
  String remoteVersion;
  String binUrl;

  if (isBeta) {
    // ── Beta channel: GitHub API /releases?per_page=1 ─────────
    // Returns the newest release including pre-releases (which
    // /releases/latest skips). Parse "tag_name" from the JSON array.
    http.begin(client, FOTA_RELEASES_API_URL);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(FOTA_VERSION_TIMEOUT_MS);
    http.addHeader("User-Agent", "pump-controller-esp32");
    http.addHeader("Accept",     "application/vnd.github.v3+json");
    int code = http.GET();

    if (code != 200) {
      logf("FOTA      — API request failed (HTTP %d): %s\n", code, FOTA_RELEASES_API_URL);
      http.end();
      return;
    }

    String body = http.getString();
    http.end();

    // Parse first "tag_name":"..." from the JSON array
    int tagIdx = body.indexOf("\"tag_name\":\"");
    if (tagIdx < 0) {
      logf("FOTA      — no releases found in API response\n");
      return;
    }
    int start = tagIdx + 12;   // skip past "tag_name":"
    int end   = body.indexOf("\"", start);
    if (end < 0) {
      logf("FOTA      — malformed tag_name in API response\n");
      return;
    }
    String tagFull = body.substring(start, end);  // e.g. "v1.0.2-b01"
    remoteVersion = tagFull;
    if (remoteVersion.startsWith("v")) remoteVersion = remoteVersion.substring(1);

    // Construct binary download URL using the full tag (with 'v' if present)
    char binBuf[220];
    snprintf(binBuf, sizeof(binBuf), FOTA_BIN_URL_TMPL, tagFull.c_str());
    binUrl = binBuf;

  } else {
    // ── Stable channel: version.txt from /releases/latest ─────
    http.begin(client, FOTA_VERSION_URL);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(FOTA_VERSION_TIMEOUT_MS);
    int code = http.GET();

    if (code != 200) {
      logf("FOTA      — version check failed (HTTP %d): %s\n", code, FOTA_VERSION_URL);
      http.end();
      return;
    }

    remoteVersion = http.getString();
    remoteVersion.trim();
    http.end();
    binUrl = FOTA_BIN_URL;
  }

  logf("FOTA      — local: %s  remote: %s\n",
       FIRMWARE_VERSION, remoteVersion.c_str());

  // ── Decide whether to update ──────────────────────────────
  bool shouldUpdate = false;
  if (isBeta) {
    // Beta: "different = update" ONLY when the API returned a pre-release
    // (tag contains "-b"). If no beta exists yet, the API returns the latest
    // stable — don't downgrade to it.
    bool remoteIsBeta = remoteVersion.indexOf("-b") >= 0;
    if (remoteIsBeta) {
      // dev→beta, beta→same-beta (no-op), beta→newer/older-beta
      shouldUpdate = (remoteVersion != String(FIRMWARE_VERSION));
    } else {
      // Remote is a stable release. Promote dev/beta builds; otherwise
      // update only if strictly newer.
      if (isDev) {
        logf("FOTA      — promoting dev/beta build to stable\n");
        shouldUpdate = true;
      } else {
        shouldUpdate = strcmp(remoteVersion.c_str(), FIRMWARE_VERSION) > 0;
      }
    }
  } else {
    // Stable: update only if remote is strictly newer.
    shouldUpdate = strcmp(remoteVersion.c_str(), FIRMWARE_VERSION) > 0;
    // Also promote dev/beta builds when switching channel back to stable.
    if (!shouldUpdate && isDev && remoteVersion.length() > 0) {
      logf("FOTA      — promoting dev/beta build to stable\n");
      shouldUpdate = true;
    }
  }

  if (!shouldUpdate) {
    logf("FOTA      — firmware is current, no update needed\n");
    return;
  }

  logf("FOTA      — update available: %s -> %s, downloading...\n",
       FIRMWARE_VERSION, remoteVersion.c_str());

  // Stop all pumps and disconnect MQTT before flashing
  for (int i = 0; i < cfg.pumpCount; i++) stopPump(i);
  if (mqtt.connected()) mqtt.disconnect();

  // Extend timeouts for the binary download
  client.setTimeout(FOTA_DL_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(FOTA_DL_TIMEOUT_MS / 1000);

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onStart([]()      { logf("FOTA      — flashing...\n"); });
  httpUpdate.onEnd([]()        { logf("FOTA      — flash complete\n"); });
  httpUpdate.onError([](int e) { logf("FOTA      — error: %d\n", e); });
  httpUpdate.onProgress([](int cur, int tot) {
    Serial.printf("FOTA      — %d%%\r", (cur * 100) / tot);
  });

  t_httpUpdate_return result = httpUpdate.update(client, binUrl.c_str());

  switch (result) {
    case HTTP_UPDATE_FAILED:
      logf("FOTA      — failed: %s\n", httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      logf("FOTA      — no update\n");
      break;
    case HTTP_UPDATE_OK:
      break;  // device restarts automatically
  }
}

// ═══════════════════════════════════════════════════════════
//  BOOT BUTTON — hold BOOT_HOLD_MS to enter portal
// ═══════════════════════════════════════════════════════════

void checkBootButton() {
  if (digitalRead(BTN_BOOT) == LOW) {
    logf("Boot btn  — held, waiting %dms for portal trigger\n", BOOT_HOLD_MS);
    delay(BOOT_HOLD_MS);
    if (digitalRead(BTN_BOOT) == LOW) {
      logf("Boot btn  — portal triggered\n");
      startPortal();
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(200);
  logf("Boot      — %s\n", FIRMWARE_VERSION);

  pinMode(BTN_BOOT, INPUT_PULLUP);

  loadConfig();
  logf("Config    — unit=%d ssid='%s' mqtt=%s:%d pumps=%d\n",
    cfg.unitNumber, cfg.wifiSSID, cfg.mqttBroker, cfg.mqttPort, cfg.pumpCount);
  validateConfig();

  if (!configLoaded) {
    logf("Config    — not configured, starting portal\n");
    buildDerivedConfig();
    startPortal();
    return;  // startPortal() restarts — this line never reached
  }

  buildDerivedConfig();
  initPumpPins();

  // Water level reed switch — INPUT_PULLUP; take an initial reading so
  // the debounce state is seeded and the first publish reflects reality.
  if (cfg.waterLevelPin >= 0) {
    pinMode(cfg.waterLevelPin, INPUT_PULLUP);
    waterLevelPinLast = digitalRead(cfg.waterLevelPin);
    waterLevelStableMs = millis();
    waterLevelLow = (waterLevelPinLast == LOW);
    logf("Water     — pin GPIO%d, initial state: %s\n",
      cfg.waterLevelPin, waterLevelLow ? "LOW" : "OK");
  }

  checkBootButton();

  if (!connectWiFi()) {
    logf("WiFi      — failed, starting portal\n");
    startPortal();
    return;
  }

  // NTP — sync before opening syslog so buffered messages get real timestamps.
  // Matches moisture-sensor pattern: NTP first, syslogFlush after.
  // No syslog UDP activity during the wait, so lwIP can service SNTP freely.
  {
    // DNS probe — log whether pool.ntp.org resolves and to what IP,
    // so we can tell apart "DNS broken" from "UDP/firewall blocked".
    IPAddress ntpIP;
    int dnsResult = WiFi.hostByName(NTP_SERVER, ntpIP);
    if (dnsResult == 1) {
      logf("NTP       — %s → %s\n", NTP_SERVER, ntpIP.toString().c_str());
    } else {
      logf("NTP       — DNS failed for '%s' (result=%d)\n", NTP_SERVER, dnsResult);
    }

    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
    logf("NTP       — syncing\n");
    struct tm t;
    bool synced = false;
    unsigned long ntpStart = millis();
    while (!synced) {
      if (getLocalTime(&t, 0)) {   // 0 ms internal wait — poll, don't block
        synced = true;
      } else if (millis() - ntpStart >= NTP_TIMEOUT_MS) {
        logf("NTP       — timed out, timestamps will be approximate\n");
        break;
      } else {
        delay(200);
      }
    }
    if (synced) {
      logf("NTP       — %04d-%02d-%02dT%02d:%02d:%02dZ\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    }
  }

  // Bind UDP socket before first send, then flush buffered boot messages.
  // (Mirrors moisture-sensor: syslogUdp.begin(0) → syslogFlush)
  syslogUdp.begin(0);
  logf("Syslog    — flushing\n");
  syslogFlush();

  // FOTA check (once per boot)
  checkForUpdate();

  // MQTT initial connect
  mqttConnect();
}

// ═══════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════

static unsigned long lastHeartbeat   = 0;
static unsigned long lastMqttAttempt = 0;
static unsigned long lastFotaCheck   = 0;  // 0 = checked on boot via setup()

void loop() {
  // ── MQTT reconnect with cooldown ──────────────────────────
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttAttempt >= MQTT_RECONNECT_COOLDOWN_MS) {
      lastMqttAttempt = now;
      logf("MQTT      — reconnecting\n");
      mqttConnect();
    }
  } else {
    mqtt.loop();
    mqtt.loop();  // drain up to 2 queued messages per iteration
  }

  // ── Process deferred pump command ─────────────────────────
  if (pendingPumpCmd.active) {
    pendingPumpCmd.active = false;
    int idx = pendingPumpCmd.idx;
    if (pendingPumpCmd.water) {
      startPump(idx);
    } else {
      stopPump(idx);
      logf("Pump %d   — stopped by command\n", idx + 1);
    }
    if (mqtt.connected()) publishPumpState(idx);
  }

  // ── Pump auto-stop (safety cap / duration expiry / water LOW) ────
  for (int i = 0; i < cfg.pumpCount; i++) {
    if (!pumpSlot[i].running) continue;
    if (millis() - pumpSlot[i].startMs >= (unsigned long)pumpSlot[i].durationMs) {
      stopPump(i);
      logf("Pump %d   — stopped (duration elapsed)\n", i + 1);
      if (mqtt.connected()) publishPumpState(i);
    } else if (cfg.waterLevelPin >= 0 && digitalRead(cfg.waterLevelPin) == LOW) {
      // Raw pin check — no debounce — so a running pump is cut immediately
      // when the reed closes, regardless of where the debounced state machine is.
      stopPump(i);
      logf("Pump %d   — stopped (water LOW, raw pin)\n", i + 1);
      if (mqtt.connected()) publishPumpState(i);
    }
  }

  // ── Heartbeat ─────────────────────────────────────────────
  if (mqtt.connected() && millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    publishUnitState();
  }

  // ── Water level sensor ────────────────────────────────────
  updateWaterLevel();

  // ── FOTA check — hourly or on channel change ──────────────
  // Skip if any pump is running — don't interrupt an active water cycle.
  {
    bool anyRunning = false;
    for (int i = 0; i < cfg.pumpCount; i++) anyRunning |= pumpSlot[i].running;
    if (!anyRunning) {
      if (pendingFotaCheck) {
        pendingFotaCheck = false;
        lastFotaCheck = millis();
        checkForUpdate();
      } else if (millis() - lastFotaCheck >= FOTA_CHECK_INTERVAL_MS) {
        lastFotaCheck = millis();
        checkForUpdate();
      }
    }
  }

  // ── Deferred config update ────────────────────────────────
  if (pendingConfigUpdate) {
    pendingConfigUpdate = false;
    applyConfigUpdate(pendingConfigPayload);
  }

  // ── Deferred restart ──────────────────────────────────────
  if (pendingRestart) {
    logf("Cmd       — restarting\n");
    for (int i = 0; i < cfg.pumpCount; i++) stopPump(i);
    if (mqtt.connected()) mqtt.disconnect();
    delay(500);
    ESP.restart();
  }

  // ── Deferred config wipe ──────────────────────────────────
  if (pendingReset) {
    logf("Cmd       — wiping config and restarting into portal\n");
    for (int i = 0; i < cfg.pumpCount; i++) stopPump(i);
    if (mqtt.connected()) mqtt.disconnect();
    clearConfig();
    delay(500);
    ESP.restart();
  }
}
