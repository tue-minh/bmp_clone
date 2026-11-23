#include "app_config.h"
#include "config.h"

#include <EEPROM.h>
#include <WebServer.h>
#include <WiFi.h>

static const uint32_t kEepromMagic   = 0x45535034UL;  // "ESP4"
static const uint16_t kEepromVersion = 3;

struct EepromRecord {
  uint32_t  magic;
  uint16_t  version;
  uint16_t  reserved;
  AppConfig cfg;
  uint32_t  crc32;
};

static AppConfig*     s_cfg = nullptr;
static WebConfigHooks s_hooks = {};
static WebServer      s_server(80);
static String         s_notice;
static bool           s_eepromReady = false;

static uint32_t calcCrc32(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return ~crc;
}

static bool ensureEeprom() {
  if (!s_eepromReady) {
    s_eepromReady = EEPROM.begin(sizeof(EepromRecord));
  }
  return s_eepromReady;
}

void configDefaults(AppConfig* cfg) {
  memset(cfg, 0, sizeof(*cfg));
  strlcpy(cfg->wifiSsid, WIFI_SSID, sizeof(cfg->wifiSsid));
  strlcpy(cfg->wifiPass, WIFI_PASSWORD, sizeof(cfg->wifiPass));
  cfg->wifiRetryMs   = WIFI_RETRY_INTERVAL_MS;
  cfg->wifiTimeoutMs = WIFI_CONNECT_TIMEOUT_MS;

  strlcpy(cfg->apSsid, AP_SSID, sizeof(cfg->apSsid));
  strlcpy(cfg->apPass, AP_PASSWORD, sizeof(cfg->apPass));

  strlcpy(cfg->mqttHost, ESP32_MQTT_BROKER_HOST, sizeof(cfg->mqttHost));
  cfg->mqttPort            = ESP32_MQTT_BROKER_PORT;
  cfg->mqttSslEnable       = (EC600N_MQTT_SSL_ENABLE != 0);
  cfg->mqttTlsInsecure     = (MQTT_TLS_INSECURE != 0);
  cfg->mqttTlsHandshakeSec = MQTT_TLS_HANDSHAKE_SEC;
  strlcpy(cfg->deviceId,     ESP32_DEVICE_ID,      sizeof(cfg->deviceId));
  strlcpy(cfg->mqttClientId, ESP32_MQTT_CLIENT_ID, sizeof(cfg->mqttClientId));
  strlcpy(cfg->mqttUser,     ESP32_MQTT_USERNAME,  sizeof(cfg->mqttUser));
  strlcpy(cfg->mqttPass,     EC600N_MQTT_PASSWORD, sizeof(cfg->mqttPass));
  cfg->mqttKeepalive = EC600N_MQTT_KEEPALIVE_SEC;
  cfg->mqttRetryMs   = MQTT_RETRY_INTERVAL_MS;

  strlcpy(cfg->topicFlash,       TOPIC_FLASH_STM32,  sizeof(cfg->topicFlash));
  strlcpy(cfg->topicResetStm32,  TOPIC_RESET_STM32,  sizeof(cfg->topicResetStm32));
  strlcpy(cfg->topicCheckStm32,  TOPIC_CHECK_STM32,  sizeof(cfg->topicCheckStm32));
  strlcpy(cfg->topicResetEc600n, TOPIC_RESET_EC600N, sizeof(cfg->topicResetEc600n));
  strlcpy(cfg->topicResetEsp32,  TOPIC_RESET_ESP32,  sizeof(cfg->topicResetEsp32));
  strlcpy(cfg->topicStatus,      TOPIC_STATUS,       sizeof(cfg->topicStatus));

  strlcpy(cfg->defaultFwUrl, FIRMWARE_DEFAULT_URL, sizeof(cfg->defaultFwUrl));
  cfg->httpTimeoutMs = HTTP_TIMEOUT_MS;
  cfg->httpsInsecure = (HTTPS_DOWNLOAD_INSECURE != 0);
}

void configLoad(AppConfig* cfg) {
  configDefaults(cfg);
  if (!ensureEeprom()) {
    Serial.println("[CFG] EEPROM.begin failed, using defaults");
    return;
  }

  EepromRecord rec;
  EEPROM.get(0, rec);
  uint32_t expectedCrc = calcCrc32(&rec.cfg, sizeof(rec.cfg));
  if (rec.magic == kEepromMagic && rec.version == kEepromVersion && rec.crc32 == expectedCrc) {
    *cfg = rec.cfg;
    Serial.printf("[CFG] loaded from EEPROM (%u bytes, CRC=0x%08lX)\n",
                  (unsigned)sizeof(rec), (unsigned long)rec.crc32);
  } else {
    Serial.println("[CFG] EEPROM uninitialized, writing defaults");
    configSave(cfg);
  }
}

bool configSave(const AppConfig* cfg) {
  if (!ensureEeprom()) return false;
  EepromRecord rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic   = kEepromMagic;
  rec.version = kEepromVersion;
  rec.cfg     = *cfg;
  rec.crc32   = calcCrc32(&rec.cfg, sizeof(rec.cfg));
  EEPROM.put(0, rec);
  bool ok = EEPROM.commit();
  Serial.printf("[CFG] EEPROM save %s (CRC=0x%08lX)\n",
                ok ? "OK" : "FAILED", (unsigned long)rec.crc32);
  return ok;
}

void configResetDefaults(AppConfig* cfg) {
  configDefaults(cfg);
  configSave(cfg);
}

// ---------------------------------------------------------------------------
// Minimal built-in setup website

static String htmlEscape(const char* s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s) + 8);
  for (; *s; s++) {
    switch (*s) {
      case '&':  out += F("&amp;"); break;
      case '<':  out += F("&lt;"); break;
      case '>':  out += F("&gt;"); break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default:   out += *s; break;
    }
  }
  return out;
}

static void addInput(String& html, const char* label, const char* name,
                     const String& val, const char* type = "text") {
  html += F("<label>");
  html += label;
  html += F("<input type='");
  html += type;
  html += F("' name='");
  html += name;
  html += F("' value='");
  html += htmlEscape(val.c_str());
  html += F("'></label>");
}

static void addCheckbox(String& html, const char* label, const char* name, bool checked) {
  html += F("<label><input type='checkbox' name='");
  html += name;
  html += F("' value='1'");
  if (checked) html += F(" checked");
  html += F("> ");
  html += label;
  html += F("</label>");
}

static void handleRoot() {
  const bool wifiUp  = s_hooks.isWifiUp && s_hooks.isWifiUp();
  const bool mqttUp  = s_hooks.isMqttUp && s_hooks.isMqttUp();
  const bool stm32Up = s_hooks.isStm32Up && s_hooks.isStm32Up();
  const uint32_t dp  = s_hooks.getStm32Dp ? s_hooks.getStm32Dp() : 0;

  String h;
  h.reserve(4800);
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ESP32 SWD Config</title><style>"
         "body{font-family:sans-serif;max-width:480px;margin:12px auto;padding:0 12px;font-size:14px;color:#222}"
         "h3{margin:8px 0}fieldset{border:1px solid #ccc;border-radius:6px;padding:10px;margin:10px 0}"
         "legend{font-weight:bold;padding:0 4px}label{display:block;margin:6px 0}"
         "input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:6px;margin-top:2px}"
         ".st{background:#f5f5f5;border:1px solid #ddd;border-radius:6px;padding:8px 10px;line-height:1.5}"
         ".msg{background:#e7f5ea;border:1px solid #9bd1a7;padding:7px 10px;border-radius:6px;margin:8px 0}"
         ".row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}"
         "button{padding:8px 12px;cursor:pointer}.pri{background:#0b5ed7;color:#fff;border:0;border-radius:4px}"
         "</style></head><body><h3>ESP32 SWD Setup (EEPROM)</h3>");

  if (s_notice.length() > 0) {
    h += F("<div class='msg'>");
    h += htmlEscape(s_notice.c_str());
    h += F("</div>");
    s_notice = "";
  }

  h += F("<div class='st'><b>WiFi STA:</b> ");
  h += wifiUp ? (String("Ket noi (") + WiFi.localIP().toString() + ")") : String("Chua ket noi");
  h += F("<br><b>SoftAP:</b> ");
  h += htmlEscape(s_cfg->apSsid);
  h += F(" (");
  h += WiFi.softAPIP().toString();
  h += F(")<br><b>MQTT:</b> ");
  h += mqttUp ? F("Da ket noi") : F("Mat ket noi");
  h += F("<br><b>STM32 SWD:</b> ");
  if (stm32Up) {
    char dpBuf[28];
    snprintf(dpBuf, sizeof(dpBuf), "CONNECTED (0x%08lX)", (unsigned long)dp);
    h += dpBuf;
  } else {
    h += F("DISCONNECTED");
  }
  h += F("<br><b>EC600N (GPIO25):</b> ");
  bool needRst = s_hooks.isStm32ResetNeeded && s_hooks.isStm32ResetNeeded();
  h += needRst ? F("CAN RESET EC600N (PA4 HIGH)") : F("Binh thuong (LOW)");
  h += F("</div>");

  h += F("<form method='POST' action='/save'>"
         "<fieldset><legend>WiFi STA &amp; SoftAP</legend>");
  addInput(h, "Ten WiFi (SSID)", "w_ssid", s_cfg->wifiSsid);
  addInput(h, "Mat khau WiFi", "w_pass", s_cfg->wifiPass, "password");
  addInput(h, "Thu lai WiFi (ms)", "w_retry", String(s_cfg->wifiRetryMs), "number");
  addInput(h, "Timeout ket noi WiFi (ms)", "w_tmo", String(s_cfg->wifiTimeoutMs), "number");
  addInput(h, "Ten SoftAP phat ra", "ap_ssid", s_cfg->apSsid);
  addInput(h, "Mat khau SoftAP (>=8 ky tu, rong = mo)", "ap_pass", s_cfg->apPass, "password");

  h += F("</fieldset><fieldset><legend>MQTT / MQTTS</legend>");
  addInput(h, "Broker Host", "m_host", s_cfg->mqttHost);
  addInput(h, "Port", "m_port", String(s_cfg->mqttPort), "number");
  addCheckbox(h, "Bat SSL/TLS (MQTTS)", "m_ssl", s_cfg->mqttSslEnable);
  addCheckbox(h, "Bo qua kiem tra TLS CA (chi debug)", "m_insec", s_cfg->mqttTlsInsecure);
  addInput(h, "Timeout TLS/Socket (giay)", "m_tls", String(s_cfg->mqttTlsHandshakeSec), "number");
  addInput(h, "Device ID", "dev_id", s_cfg->deviceId);
  addInput(h, "Client ID", "m_cid", s_cfg->mqttClientId);
  addInput(h, "Username", "m_user", s_cfg->mqttUser);
  addInput(h, "Password", "m_pass", s_cfg->mqttPass, "password");
  addInput(h, "Keepalive (giay)", "m_ka", String(s_cfg->mqttKeepalive), "number");
  addInput(h, "Thu lai MQTT (ms)", "m_retry", String(s_cfg->mqttRetryMs), "number");

  h += F("</fieldset><fieldset><legend>MQTT Topics</legend>");
  addInput(h, "Topic Flash STM32", "t_flash", s_cfg->topicFlash);
  addInput(h, "Topic Reset STM32", "t_rst_s", s_cfg->topicResetStm32);
  addInput(h, "Topic Check STM32", "t_chk_s", s_cfg->topicCheckStm32);
  addInput(h, "Topic Reset EC600N", "t_rst_ec", s_cfg->topicResetEc600n);
  addInput(h, "Topic Reset ESP32", "t_rst_e", s_cfg->topicResetEsp32);
  addInput(h, "Topic Status", "t_stat", s_cfg->topicStatus);

  h += F("</fieldset><fieldset><legend>HTTP / Firmware</legend>");
  addInput(h, "URL .bin mac dinh (tuy chon)", "fw_url", s_cfg->defaultFwUrl);
  addInput(h, "HTTP Timeout (ms)", "h_tmo", String(s_cfg->httpTimeoutMs), "number");
  addCheckbox(h, "Bo qua kiem tra HTTPS CA khi tai .bin", "h_insec", s_cfg->httpsInsecure);

  h += F("</fieldset><div class='row'>"
         "<button class='pri' type='submit'>Luu EEPROM &amp; Ap dung</button>"
         "</div></form>");

  h += F("<div class='row'>"
         "<form method='POST' action='/check'><button type='submit'>Kiem tra STM32</button></form>"
         "<form method='POST' action='/reset-ec600n'><button type='submit'>Reset EC600N</button></form>"
         "<form method='POST' action='/reboot'><button type='submit'>Khoi dong lai</button></form>"
         "<form method='POST' action='/reset-cfg' onsubmit=\"return confirm('Khoi phuc mac dinh va luu EEPROM?')\">"
         "<button type='submit'>Mac dinh</button></form>"
         "<a href='/'><button type='button'>Lam moi</button></a>"
         "</div></body></html>");

  s_server.send(200, "text/html", h);
}

static void redirectHome() {
  s_server.sendHeader("Location", "/");
  s_server.send(303, "text/plain", "");
}

static void copyArgTrim(const char* name, char* dst, size_t len, bool allowEmpty = true) {
  if (!s_server.hasArg(name)) return;
  String v = s_server.arg(name);
  v.trim();
  if (!allowEmpty && v.length() == 0) return;
  strlcpy(dst, v.c_str(), len);
}

static void copyArgRaw(const char* name, char* dst, size_t len) {
  if (!s_server.hasArg(name)) return;
  strlcpy(dst, s_server.arg(name).c_str(), len);
}

static uint32_t clampU32(long v, uint32_t lo, uint32_t hi, uint32_t cur) {
  if (v < (long)lo || v > (long)hi) return cur;
  return (uint32_t)v;
}

static bool rejectIfBusy() {
  if (s_hooks.isBusy && s_hooks.isBusy()) {
    s_server.send(409, "text/plain", "Busy: flash/reset job in progress");
    return true;
  }
  return false;
}

static void handleSave() {
  if (rejectIfBusy()) return;
  char prevApSsid[sizeof(s_cfg->apSsid)];
  char prevApPass[sizeof(s_cfg->apPass)];
  strlcpy(prevApSsid, s_cfg->apSsid, sizeof(prevApSsid));
  strlcpy(prevApPass, s_cfg->apPass, sizeof(prevApPass));

  copyArgTrim("w_ssid", s_cfg->wifiSsid, sizeof(s_cfg->wifiSsid));
  copyArgRaw("w_pass", s_cfg->wifiPass, sizeof(s_cfg->wifiPass));
  s_cfg->wifiRetryMs   = clampU32(s_server.arg("w_retry").toInt(), 500, 60000, s_cfg->wifiRetryMs);
  s_cfg->wifiTimeoutMs = clampU32(s_server.arg("w_tmo").toInt(), 2000, 30000, s_cfg->wifiTimeoutMs);

  copyArgTrim("ap_ssid", s_cfg->apSsid, sizeof(s_cfg->apSsid), false);
  if (s_server.hasArg("ap_pass")) {
    String apP = s_server.arg("ap_pass");
    if (apP.length() == 0 || apP.length() >= 8) {
      strlcpy(s_cfg->apPass, apP.c_str(), sizeof(s_cfg->apPass));
    }
  }

  copyArgTrim("m_host", s_cfg->mqttHost, sizeof(s_cfg->mqttHost), false);
  s_cfg->mqttPort            = (uint16_t)clampU32(s_server.arg("m_port").toInt(), 1, 65535, s_cfg->mqttPort);
  s_cfg->mqttSslEnable       = s_server.hasArg("m_ssl");
  s_cfg->mqttTlsInsecure     = s_server.hasArg("m_insec");
  s_cfg->mqttTlsHandshakeSec = (uint16_t)clampU32(s_server.arg("m_tls").toInt(), 2, 60, s_cfg->mqttTlsHandshakeSec);
  copyArgTrim("dev_id", s_cfg->deviceId,     sizeof(s_cfg->deviceId), false);
  copyArgTrim("m_cid",  s_cfg->mqttClientId, sizeof(s_cfg->mqttClientId), false);
  copyArgTrim("m_user", s_cfg->mqttUser,     sizeof(s_cfg->mqttUser));
  copyArgRaw("m_pass",  s_cfg->mqttPass,     sizeof(s_cfg->mqttPass));
  s_cfg->mqttKeepalive = (uint16_t)clampU32(s_server.arg("m_ka").toInt(), 5, 3600, s_cfg->mqttKeepalive);
  s_cfg->mqttRetryMs   = clampU32(s_server.arg("m_retry").toInt(), 500, 60000, s_cfg->mqttRetryMs);

  copyArgTrim("t_flash",  s_cfg->topicFlash,       sizeof(s_cfg->topicFlash), false);
  copyArgTrim("t_rst_s",  s_cfg->topicResetStm32,  sizeof(s_cfg->topicResetStm32), false);
  copyArgTrim("t_chk_s",  s_cfg->topicCheckStm32,  sizeof(s_cfg->topicCheckStm32), false);
  copyArgTrim("t_rst_ec", s_cfg->topicResetEc600n, sizeof(s_cfg->topicResetEc600n), false);
  copyArgTrim("t_rst_e",  s_cfg->topicResetEsp32,  sizeof(s_cfg->topicResetEsp32), false);
  copyArgTrim("t_stat",   s_cfg->topicStatus,      sizeof(s_cfg->topicStatus), false);

  copyArgTrim("fw_url", s_cfg->defaultFwUrl, sizeof(s_cfg->defaultFwUrl));
  s_cfg->httpTimeoutMs = clampU32(s_server.arg("h_tmo").toInt(), 2000, 60000, s_cfg->httpTimeoutMs);
  s_cfg->httpsInsecure = s_server.hasArg("h_insec");

  bool apChanged = (strcmp(prevApSsid, s_cfg->apSsid) != 0) ||
                   (strcmp(prevApPass, s_cfg->apPass) != 0);

  if (configSave(s_cfg)) {
    s_notice = "Da luu thang vao EEPROM va dang ap dung cau hinh moi!";
    if (s_hooks.onApply) s_hooks.onApply(apChanged);
  } else {
    s_notice = "Loi ghi EEPROM!";
  }
  redirectHome();
}

static void handleCheck() {
  if (s_hooks.onCheckStm32 && s_hooks.onCheckStm32()) {
    s_notice = "Da gui yeu cau kiem tra STM32, bam Lam moi sau 1s.";
  } else {
    s_notice = "ESP32 dang ban, thu lai sau.";
  }
  redirectHome();
}

static void handleResetEc600n() {
  if (rejectIfBusy()) return;
  if (s_hooks.onResetEc600n) s_hooks.onResetEc600n();
  s_notice = "Da keo PING_STM32 (GPIO26) HIGH 1 lan de STM32 reset EC600N.";
  redirectHome();
}

static void handleReboot() {
  if (rejectIfBusy()) return;
  s_server.send(200, "text/html",
                "<!doctype html><html><meta charset='utf-8'><body>"
                "<p>Dang khoi dong lai ESP32...</p></body></html>");
  if (s_hooks.onReboot) s_hooks.onReboot();
}

static void handleResetCfg() {
  if (rejectIfBusy()) return;
  configResetDefaults(s_cfg);
  s_notice = "Da khoi phuc mac dinh vao EEPROM va ket noi lai.";
  if (s_hooks.onApply) s_hooks.onApply(true);
  redirectHome();
}

void webConfigInit(AppConfig* cfg, const WebConfigHooks& hooks) {
  s_cfg = cfg;
  s_hooks = hooks;
  s_server.on("/", HTTP_GET, handleRoot);
  s_server.on("/save", HTTP_POST, handleSave);
  s_server.on("/check", HTTP_POST, handleCheck);
  s_server.on("/reset-ec600n", HTTP_POST, handleResetEc600n);
  s_server.on("/reboot", HTTP_POST, handleReboot);
  s_server.on("/reset-cfg", HTTP_POST, handleResetCfg);
  s_server.onNotFound(handleRoot);
  s_server.begin();
  Serial.printf("[WEB] config page at http://%s/ and http://<STA_IP>/\n",
                WiFi.softAPIP().toString().c_str());
}

void webConfigLoop() {
  s_server.handleClient();
}
