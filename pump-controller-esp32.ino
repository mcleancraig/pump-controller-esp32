
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
//  v0.1.0
//  - Initial release: configurable N pumps (1-5), always-awake loop,
//    MQTT command/state per pump, WiFi+captive portal, syslog,
//    HA MQTT autodiscovery, FOTA, NTP, NVS config
//  - Hard safety cap: PUMP_MAX_DURATION_S — firmware-enforced, not
//    overridable via config or MQTT
//  - MQTT callback safety: no publish() inside callback; deferred via flags
// ═══════════════════════════════════════════════════════════

// Dev builds: update SHA suffix with `git rev-parse --short HEAD` after each commit.
#define FIRMWARE_VERSION "0.1.0-dev.84ba7a5"

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
const int NTP_TIMEOUT_MS            = 10000;
const int FOTA_VERSION_TIMEOUT_MS   = 8000;
const int FOTA_DL_TIMEOUT_MS        = 60000;

// ── AP credentials ────────────────────────────────────────
const char* AP_PASSWORD = "moisture";

// ── NTP ───────────────────────────────────────────────────
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 0;
const int   DST_OFFSET_S = 0;

// ── FOTA ─────────────────────────────────────────────────
const char* FOTA_VERSION_URL =
  "https://github.com/mcleancraig/pump-controller-esp32"
  "/releases/latest/download/version.txt";
const char* FOTA_BIN_URL =
  "https://github.com/mcleancraig/pump-controller-esp32"
  "/releases/latest/download/pump-controller-esp32.ino.bin";

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
} cfg;

bool configLoaded = false;

// Forward-declare _logf so logf macro compiles before first use
#define logf(fmt, ...) _logf(__func__, fmt, ##__VA_ARGS__)

void loadConfig() {
  prefs.begin("pump", true);
  cfg.unitNumber  = prefs.getInt("unitNum", 0);
  prefs.getString("wifiSSID",   cfg.wifiSSID,    sizeof(cfg.wifiSSID));
  prefs.getString("wifiPass",   cfg.wifiPassword, sizeof(cfg.wifiPassword));
  cfg.staticIP    = prefs.getBool("staticIP", false);

  if (prefs.getBytes("ip",  cfg.ip,  4) == 0) {
    cfg.ip[0] = 192; cfg.ip[1] = 168; cfg.ip[2] = 220; cfg.ip[3] = 1;
  }
  if (prefs.getBytes("gw",  cfg.gw,  4) == 0) {
    cfg.gw[0] = 192; cfg.gw[1] = 168; cfg.gw[2] =   1; cfg.gw[3] = 1;
  }
  if (prefs.getBytes("sn",  cfg.sn,  4) == 0) {
    cfg.sn[0] = 255; cfg.sn[1] = 255; cfg.sn[2] =   0; cfg.sn[3] = 0;
  }
  if (prefs.getBytes("dns", cfg.dns, 4) == 0) {
    cfg.dns[0] = 192; cfg.dns[1] = 168; cfg.dns[2] = 1; cfg.dns[3] = 1;
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

  prefs.end();

  configLoaded = (cfg.unitNumber > 0 &&
                  strlen(cfg.wifiSSID) > 0 &&
                  strlen(cfg.mqttBroker) > 0);
}

void clearConfig() {
  prefs.begin("pump", false);
  prefs.clear();
  prefs.end();
  logf("Config    — NVS cleared\n");
}

void saveConfig(const Config& c) {
  prefs.begin("pump", false);
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

  int count = min(syslogTotal, SYSLOG_LINES);
  int start = (syslogTotal >= SYSLOG_LINES) ? syslogHead : 0;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % SYSLOG_LINES;
    syslogSend(syslogBuf[idx].func, syslogBuf[idx].msg);
    delay(2);
  }
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
        <input type="number" name="ip3" id="ip3" value="220" min="0" max="255">
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

  <button type="submit">Save &amp; Restart</button>
</form>

<script>
function syncNet() {
  var n = parseInt(document.getElementById('unitNum').value)||1;
  document.getElementById('ip4').value = n;
}
function toggleNet(chk) {
  document.getElementById('network-rows').style.display = chk.checked ? '' : 'none';
}
function togglePw(btn) {
  var inp = btn.parentElement.querySelector('input');
  inp.type = inp.type === 'password' ? 'text' : 'password';
}
function updatePumpRows() {
  var count = parseInt(document.getElementById('pumpCount').value) || 1;
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

void stopPump(int idx) {
  if (idx < 0 || idx >= MAX_PUMPS) return;
  pumpSlot[idx].running = false;
  if (idx < cfg.pumpCount) {
    digitalWrite(cfg.pumpPin[idx], LOW);
  }
}

void startPump(int idx) {
  if (idx < 0 || idx >= cfg.pumpCount) return;
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
  char payload[400];
  snprintf(payload, sizeof(payload),
    "{\"mqttBroker\":\"%s\",\"mqttPort\":%d,\"mqttUser\":\"%s\","
    "\"syslogHost\":\"%s\",\"syslogPort\":%d,"
    "\"pumpCount\":%d,\"pumpPins\":%s,\"pumpDurations\":%s}",
    cfg.mqttBroker, cfg.mqttPort, cfg.mqttUser,
    cfg.syslogHost, cfg.syslogPort,
    cfg.pumpCount, pins, durs);
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

  char tmp[64];
  int  ival;

  if (extractStr(json, "mqttBroker",   tmp, sizeof(tmp))) strlcpy(cfg.mqttBroker,   tmp, sizeof(cfg.mqttBroker));
  if (extractInt(json, "mqttPort",     &ival) && isValidPort(ival)) cfg.mqttPort = ival;
  if (extractStr(json, "mqttUser",     tmp, sizeof(tmp))) strlcpy(cfg.mqttUser,     tmp, sizeof(cfg.mqttUser));
  if (extractStr(json, "mqttPassword", tmp, sizeof(tmp))) strlcpy(cfg.mqttPassword, tmp, sizeof(cfg.mqttPassword));
  if (extractStr(json, "syslogHost",   tmp, sizeof(tmp))) strlcpy(cfg.syslogHost,   tmp, sizeof(cfg.syslogHost));
  if (extractInt(json, "syslogPort",   &ival) && isValidPort(ival)) cfg.syslogPort = ival;
  if (extractInt(json, "pumpCount",    &ival) && ival >= 1 && ival <= MAX_PUMPS) cfg.pumpCount = ival;

  for (int i = 0; i < MAX_PUMPS; i++) {
    char keyPin[24], keyDur[24];
    snprintf(keyPin, sizeof(keyPin), "pumpPin%d", i);
    snprintf(keyDur, sizeof(keyDur), "pumpDuration%d", i);
    if (extractInt(json, keyPin, &ival) && ival >= 0 && ival <= 28) cfg.pumpPin[i] = ival;
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

  char payload[512];

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

  return true;
}

// ═══════════════════════════════════════════════════════════
//  FOTA
// ═══════════════════════════════════════════════════════════

void checkForUpdate() {
  // FOTA blocked on dev builds (version string contains '-')
  if (strchr(FIRMWARE_VERSION, '-') != nullptr) {
    logf("FOTA      — dev build, skipping\n");
    return;
  }

  logf("FOTA      — checking %s\n", FOTA_VERSION_URL);

  WiFiClientSecure client;
  client.setInsecure();  // TODO: embed ISRG Root X1 for proper TLS validation
  client.setTimeout(FOTA_VERSION_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(FOTA_VERSION_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(FOTA_VERSION_TIMEOUT_MS);

  if (!http.begin(client, FOTA_VERSION_URL)) {
    logf("FOTA      — HTTP begin failed\n");
    return;
  }

  int code = http.GET();
  if (code != 200) {
    logf("FOTA      — version check HTTP %d\n", code);
    http.end();
    return;
  }

  String remote = http.getString();
  http.end();
  remote.trim();

  logf("FOTA      — local=%s remote=%s\n", FIRMWARE_VERSION, remote.c_str());

  if (remote == FIRMWARE_VERSION) {
    logf("FOTA      — up to date\n");
    return;
  }

  // Stop all pumps before updating
  for (int i = 0; i < cfg.pumpCount; i++) stopPump(i);
  mqtt.disconnect();

  logf("FOTA      — updating to %s\n", remote.c_str());

  WiFiClientSecure dlClient;
  dlClient.setInsecure();
  dlClient.setTimeout(FOTA_DL_TIMEOUT_MS / 1000);
  dlClient.setHandshakeTimeout(FOTA_DL_TIMEOUT_MS / 1000);

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(dlClient, FOTA_BIN_URL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      logf("FOTA      — update failed: %s\n", httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      logf("FOTA      — no update\n");
      break;
    case HTTP_UPDATE_OK:
      logf("FOTA      — success, rebooting\n");
      break;
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

  if (!configLoaded) {
    logf("Config    — not configured, starting portal\n");
    buildDerivedConfig();
    startPortal();
    return;  // startPortal() restarts — this line never reached
  }

  buildDerivedConfig();
  initPumpPins();
  checkBootButton();

  if (!connectWiFi()) {
    logf("WiFi      — failed, starting portal\n");
    startPortal();
    return;
  }

  syslogFlush();

  // NTP
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  logf("NTP       — syncing\n");
  unsigned long ntpStart = millis();
  struct tm t;
  while (!getLocalTime(&t) && millis() - ntpStart < NTP_TIMEOUT_MS) delay(200);
  if (getLocalTime(&t)) {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
    logf("NTP       — %s\n", ts);
  } else {
    logf("NTP       — sync timeout\n");
  }

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

  // ── Pump auto-stop (safety cap / duration expiry) ─────────
  for (int i = 0; i < cfg.pumpCount; i++) {
    if (pumpSlot[i].running &&
        millis() - pumpSlot[i].startMs >= (unsigned long)pumpSlot[i].durationMs) {
      stopPump(i);
      logf("Pump %d   — stopped (duration elapsed)\n", i + 1);
      if (mqtt.connected()) publishPumpState(i);
    }
  }

  // ── Heartbeat ─────────────────────────────────────────────
  if (mqtt.connected() && millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    publishUnitState();
  }

  // ── Periodic FOTA check (hourly) ──────────────────────────
  // Skip if any pump is running — don't interrupt an active water cycle.
  if (millis() - lastFotaCheck >= FOTA_CHECK_INTERVAL_MS) {
    bool anyRunning = false;
    for (int i = 0; i < cfg.pumpCount; i++) anyRunning |= pumpSlot[i].running;
    if (!anyRunning) {
      lastFotaCheck = millis();
      checkForUpdate();  // reboots on successful update; returns if already up to date
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
