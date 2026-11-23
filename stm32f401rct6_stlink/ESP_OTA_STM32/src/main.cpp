// ESP32-WROOM-32D -> STM32F401RCT6 remote SWD programmer.
//
//  * WiFi STA + lightweight SoftAP config portal (192.168.4.1 / STA IP)
//  * MQTTS commands (topics in mqtt.md / config.h)
//  * .bin download over HTTP/HTTPS into LittleFS
//  * SWD flashing in a dedicated FreeRTOS worker task fed by a queue
//  * LED D2 (GPIO2) shows WiFi / MQTT state

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "app_config.h"
#include "config.h"
#include "stm32_flash.h"
#include "swd.h"

// ---------------------------------------------------------------------------
// Shared state

static AppConfig         g_cfg;
static volatile bool     g_wifiUp = false;
static volatile bool     g_mqttUp = false;
static volatile uint32_t g_wifiPulseUntil = 0;
static volatile bool     g_busy = false;          // a flash/reset/check job is queued or running
static volatile bool     g_finalResultPending = false;
static volatile bool     g_stm32LinkUp = false;
static volatile uint32_t g_stm32DpId = 0;

#define FW_TAG                     "ota-reliability-v1"
#define DOWNLOAD_TOTAL_TIMEOUT_MS  90000UL

enum JobType : uint8_t { JOB_FLASH, JOB_RESET_STM32, JOB_CHECK_STM32 };
struct Job {
  JobType type;
  char    url[512];
};

static QueueHandle_t g_jobQueue;
static QueueHandle_t g_statusQueue;
#define STATUS_LEN 192

static WiFiClient       g_mqttPlain;
static WiFiClientSecure g_mqttNet;
static PubSubClient     g_mqtt;

static uint32_t g_lastWifiAttempt = 0;
static uint32_t g_wifiNextAttempt = 0;
static volatile bool g_wifiConnecting = false;
static uint32_t g_mqttNextAttempt = 0;
static uint32_t g_stm32ResetUntil = 0;
static uint32_t g_restartAt = 0;
static uint32_t g_pingPulseUntil = 0;
static bool     g_stm32ResetNeeded = false;

static void statusf(const char* fmt, ...);
static void statusFinalf(const char* fmt, ...);

static void triggerEc600nResetPulse() {
  if (g_busy) {
    statusf("reset ec600n: refused, flash/reset job in progress");
    return;
  }
#if PING_STM32 >= 0
  digitalWrite(PING_STM32, HIGH);
  uint32_t until = millis() + PING_EC_RESET_PULSE_MS;
  if (until == 0) until = 1;
  g_pingPulseUntil = until;
  statusf("reset ec600n: HIGH %d ms on PING_STM32 (GPIO%d)",
          PING_EC_RESET_PULSE_MS, PING_STM32);
#endif
}

// ---------------------------------------------------------------------------
// Status: Serial log + queued for MQTT publish (published from loop()).

static void enqueueStatusText(const char* buf, bool isFinal) {
  Serial.printf("[STATUS] %s\n", buf);
  if (!g_statusQueue) return;

  if (isFinal) {
    g_finalResultPending = true;
    /* Reserve last slot for final result; drop oldest non-final if completely full */
    if (uxQueueSpacesAvailable(g_statusQueue) == 0) {
      char dummy[STATUS_LEN];
      xQueueReceive(g_statusQueue, dummy, 0);
    }
    xQueueSend(g_statusQueue, buf, 0);
  } else {
    /* Always leave at least 1 slot reserved for final result */
    if (uxQueueSpacesAvailable(g_statusQueue) > 1) {
      xQueueSend(g_statusQueue, buf, 0);
    }
  }
}

static void statusf(const char* fmt, ...) {
  char buf[STATUS_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  enqueueStatusText(buf, false);
}

static void statusFinalf(const char* fmt, ...) {
  char buf[STATUS_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  enqueueStatusText(buf, true);
}

static void publishPendingStatus() {
  if (!g_mqtt.connected() || !g_statusQueue) return;
  char buf[STATUS_LEN];
  while (xQueuePeek(g_statusQueue, buf, 0) == pdTRUE) {
    if (!g_mqtt.publish(g_cfg.topicStatus, buf)) {
      break; /* Keep head of queue on publish failure to preserve order */
    }
    xQueueReceive(g_statusQueue, buf, 0);
  }
  if (uxQueueMessagesWaiting(g_statusQueue) == 0) {
    g_finalResultPending = false;
  }
}

// ---------------------------------------------------------------------------
// LED D2 controller (own task, non-blocking for the rest of the system)
//   WiFi down                 -> OFF
//   WiFi just connected       -> solid ON for LED_WIFI_PULSE_MS, then short OFF gap
//   WiFi OK, MQTT down        -> solid ON
//   WiFi OK, MQTT connected   -> slow blink

static void ledTask(void*) {
  for (;;) {
    const uint32_t now = millis();
    const int32_t toPulseEnd = (int32_t)(g_wifiPulseUntil - now);
    bool on;
    if (!g_wifiUp)                       on = false;
    else if (toPulseEnd > 0)             on = true;
    else if (toPulseEnd > -LED_GAP_MS)   on = false;
    else if (!g_mqttUp)                  on = true;
    else                                 on = (now / LED_SLOW_BLINK_MS) & 1;
    digitalWrite(LED_PIN, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------------------------------------------------
// HTTP/HTTPS download into LittleFS

// Stream adapter that writes into a File and refuses to grow past a limit,
// so HTTPClient::writeToStream() aborts on oversize (chunked or not).
class LimitedFileStream : public Stream {
 public:
  LimitedFileStream(File& f, size_t limit) : _f(f), _limit(limit) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t len) override {
    if (_written + len > _limit) { overflow = true; return 0; }
    size_t n = _f.write(buf, len);
    _written += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { _f.flush(); }
  size_t written() const { return _written; }
  bool overflow = false;

 private:
  File&  _f;
  size_t _limit;
  size_t _written = 0;
};

static void redactUrlForLog(const char* url, char* out, size_t outLen) {
  if (!url || !out || outLen == 0) return;
  const char* q = strchr(url, '?');
  size_t copyLen = q ? (size_t)(q - url) : strlen(url);
  if (copyLen >= outLen) copyLen = outLen - 1;
  memcpy(out, url, copyLen);
  out[copyLen] = '\0';
}

static bool downloadFirmware(const char* url, uint32_t* outSize, char* err, size_t errLen) {
  const uint32_t dlStartMs = millis();
  const bool https = strncmp(url, "https://", 8) == 0;
  if (!https && strncmp(url, "http://", 7) != 0) {
    snprintf(err, errLen, "Invalid URL scheme (need http:// or https://)");
    return false;
  }

  WiFiClient plain;
  WiFiClientSecure secure;
  if (https) {
    if (g_cfg.httpsInsecure) {
      secure.setInsecure();
    } else {
      secure.setCACert(MQTT_ROOT_CA);
    }
    uint32_t tlsSec = g_cfg.httpTimeoutMs / 1000UL;
    if (tlsSec == 0) tlsSec = 1;
    secure.setHandshakeTimeout(tlsSec);
  }

  HTTPClient http;
  http.useHTTP10(true);
  http.setReuse(false);
  http.setConnectTimeout(g_cfg.httpTimeoutMs);
  http.setTimeout(g_cfg.httpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(https ? (WiFiClient&)secure : plain, url)) {
    snprintf(err, errLen, "HTTP begin failed");
    return false;
  }

  char safeUrl[96];
  redactUrlForLog(url, safeUrl, sizeof(safeUrl));
  Serial.printf("[HTTP] GET %s (heap=%u, stack_hwm=%u)\n",
                safeUrl, (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(err, errLen, "HTTP GET failed: %d %s", code,
             code < 0 ? HTTPClient::errorToString(code).c_str() : "");
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len < 8) {
    snprintf(err, errLen, "Invalid/missing Content-Length (%d B)", len);
    http.end();
    return false;
  }
  if (len > (int)STM32_FLASH_SIZE) {
    snprintf(err, errLen, "Firmware too large: %d B > %lu B", len, (unsigned long)STM32_FLASH_SIZE);
    http.end();
    return false;
  }

  statusf("flash: HTTP %d (%d B)", code, len);

  LittleFS.remove(FIRMWARE_PATH);
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if ((size_t)len + 4096 > freeBytes) {
    snprintf(err, errLen, "Not enough LittleFS space (%u free)", (unsigned)freeBytes);
    http.end();
    return false;
  }

  File f = LittleFS.open(FIRMWARE_PATH, "w");
  if (!f) {
    snprintf(err, errLen, "Cannot create %s", FIRMWARE_PATH);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  static uint8_t rxBuf[1024];
  uint32_t got = 0;
  uint32_t total = (uint32_t)len;
  uint32_t lastDataMs = millis();
  int lastPct = -1;
  bool dlOk = true;

  while (got < total) {
    uint32_t now = millis();
    if ((uint32_t)(now - dlStartMs) >= DOWNLOAD_TOTAL_TIMEOUT_MS) {
      snprintf(err, errLen, "Download total timeout (>%lus) at %lu/%lu B",
               (unsigned long)(DOWNLOAD_TOTAL_TIMEOUT_MS / 1000UL),
               (unsigned long)got, (unsigned long)total);
      dlOk = false;
      break;
    }

    int avail = stream->available();
    if (avail > 0) {
      size_t toRead = (size_t)avail;
      if (toRead > sizeof(rxBuf)) toRead = sizeof(rxBuf);
      if (got + toRead > total) toRead = (size_t)(total - got);

      int rd = stream->readBytes(rxBuf, toRead);
      if (rd > 0) {
        size_t wr = f.write(rxBuf, (size_t)rd);
        if (wr != (size_t)rd) {
          snprintf(err, errLen, "LittleFS write failed at %lu/%lu B",
                   (unsigned long)got, (unsigned long)total);
          dlOk = false;
          break;
        }
        got += (uint32_t)rd;
        lastDataMs = millis();

        int pct = (int)((uint64_t)got * 100ULL / total);
        if (pct / 25 != lastPct / 25 || pct == 100) {
          if (pct != lastPct) {
            statusf("flash: download %d%% (%lu/%lu B)", pct,
                    (unsigned long)got, (unsigned long)total);
            lastPct = pct;
          }
        }
      }
    } else {
      if (!stream->connected()) {
        snprintf(err, errLen, "Connection closed early at %lu/%lu B",
                 (unsigned long)got, (unsigned long)total);
        dlOk = false;
        break;
      }
      if ((uint32_t)(now - lastDataMs) >= g_cfg.httpTimeoutMs) {
        snprintf(err, errLen, "Download idle timeout (%lums) at %lu/%lu B",
                 (unsigned long)g_cfg.httpTimeoutMs,
                 (unsigned long)got, (unsigned long)total);
        dlOk = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  f.flush();
  f.close();
  http.end();

  if (!dlOk || got != total) {
    if (dlOk && got != total) {
      snprintf(err, errLen, "Download incomplete: %lu/%lu B",
               (unsigned long)got, (unsigned long)total);
    }
    LittleFS.remove(FIRMWARE_PATH);
    return false;
  }

  *outSize = got;
  return true;
}

// ---------------------------------------------------------------------------
// Worker task: download + SWD flash / SWD reset. Never runs in MQTT callback.

static void flashProgress(const char* stage, uint32_t done, uint32_t total) {
  static const char* lastStage = nullptr;
  static int lastPct = -1;
  int pct = total ? (int)((uint64_t)done * 100 / total) : 100;
  if (stage != lastStage) { lastStage = stage; lastPct = -1; }
  if (pct / 25 != lastPct / 25 || pct == 100) {
    if (pct != lastPct) statusf("flash: %s %d%%", stage, pct);
    lastPct = pct;
  }
}

static void reportStm32Probe() {
  static char err[STATUS_LEN - 32];
  Stm32Info info;
  if (stm32Probe(&info, err, sizeof(err))) {
    g_stm32LinkUp = true;
    g_stm32DpId = info.dpIdcode;
    statusf("stm32: CONNECTED DP=0x%08lX DEV_ID=0x%03X REV=0x%04X Flash=%luKB RDP=0x%02X",
            (unsigned long)info.dpIdcode, info.devId, info.revId,
            (unsigned long)info.flashSizeKB, info.rdpLevel);
  } else {
    g_stm32LinkUp = (info.dpIdcode != 0);
    g_stm32DpId = info.dpIdcode;
    statusf("stm32: DISCONNECTED - %s", err);
  }
}

static void workerTask(void*) {
  static Job job;
  static char err[STATUS_LEN - 32];
  bool linkCheckedOnce = false;

  for (;;) {
    if (xQueueReceive(g_jobQueue, &job, pdMS_TO_TICKS(3000)) != pdTRUE) {
      uint32_t dpId = 0;
      bool up = stm32CheckLink(&dpId);
      if (!linkCheckedOnce || up != g_stm32LinkUp || dpId != g_stm32DpId) {
        linkCheckedOnce = true;
        g_stm32LinkUp = up;
        g_stm32DpId = dpId;
        if (up) statusf("stm32: CONNECTED DP=0x%08lX", (unsigned long)dpId);
        else    statusf("stm32: DISCONNECTED (no SWD ACK)");
      }
      continue;
    }
    err[0] = 0;

    if (job.type == JOB_FLASH) {
      uint32_t t0 = millis();
      uint32_t size = 0;
      char safeUrl[80];
      redactUrlForLog(job.url, safeUrl, sizeof(safeUrl));
      statusf("flash: downloading %s", safeUrl);
      if (!downloadFirmware(job.url, &size, err, sizeof(err))) {
        statusFinalf("flash: FAILED download - %s", err);
      } else {
        statusf("flash: downloaded %lu B, flashing STM32 over SWD", (unsigned long)size);
        if (stm32FlashFile(FIRMWARE_PATH, flashProgress, err, sizeof(err))) {
          g_stm32LinkUp = true;
          statusFinalf("flash: OK %lu B in %lu ms verified; STM32 reset requested",
                       (unsigned long)size, (unsigned long)(millis() - t0));
        } else {
          statusFinalf("flash: FAILED - %s", err);
        }
      }
      Serial.printf("[WORKER] done (heap=%u, stack_hwm=%u)\n",
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    } else if (job.type == JOB_RESET_STM32) {
      if (stm32Reset(err, sizeof(err))) statusFinalf("reset stm32: OK (SWD)");
      else                              statusFinalf("reset stm32: FAILED - %s", err);
    } else if (job.type == JOB_CHECK_STM32) {
      reportStm32Probe();
      linkCheckedOnce = true;
    }

    g_busy = false;
  }
}

static bool queueJob(JobType type, const char* url) {
  if (g_busy || g_finalResultPending) {
    statusf("busy: previous job/result pending, command ignored");
    return false;
  }
  static Job job;
  job.type = type;
  strlcpy(job.url, url ? url : "", sizeof(job.url));
  g_busy = true;
  if (xQueueSend(g_jobQueue, &job, 0) != pdTRUE) {
    g_busy = false;
    statusf("job queue full, command ignored");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// MQTT

// Accepts: plain URL, JSON {"url":"..."}, or empty/other text (default URL).
static String extractUrl(const String& payload) {
  String p = payload;
  p.trim();
  if (p.startsWith("{")) {
    int k = p.indexOf("\"url\"");
    if (k >= 0) {
      int colon = p.indexOf(':', k + 5);
      int q1 = colon >= 0 ? p.indexOf('"', colon + 1) : -1;
      int q2 = q1 >= 0 ? p.indexOf('"', q1 + 1) : -1;
      if (q1 >= 0 && q2 > q1) return p.substring(q1 + 1, q2);
    }
    return String(g_cfg.defaultFwUrl);
  }
  if (p.startsWith("http://") || p.startsWith("https://")) return p;
  return String(g_cfg.defaultFwUrl);
}

static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] <- %s (%u B)\n", topic, length);

  if (strcmp(topic, g_cfg.topicFlash) == 0) {
    String url = extractUrl(msg);
    if (url.length() == 0) {
      statusf("flash: no .bin URL in payload and default URL is empty");
      return;
    }
    queueJob(JOB_FLASH, url.c_str());
  } else if (strcmp(topic, g_cfg.topicResetStm32) == 0) {
    queueJob(JOB_RESET_STM32, nullptr);
  } else if (strcmp(topic, g_cfg.topicCheckStm32) == 0) {
    queueJob(JOB_CHECK_STM32, nullptr);
  } else if (strcmp(topic, g_cfg.topicResetEc600n) == 0) {
    triggerEc600nResetPulse();
  } else if (strcmp(topic, g_cfg.topicResetEsp32) == 0) {
    if (g_busy) {
      statusf("reset esp32: refused, flash job in progress");
      return;
    }
    statusf("reset esp32: restarting");
    g_restartAt = millis() + 500;
    if (g_restartAt == 0) g_restartAt = 1;
  }
}

static bool mqttConnect() {
  Serial.printf("[MQTT] connecting to %s:%u (%s) as %s ...\n", g_cfg.mqttHost,
                g_cfg.mqttPort, g_cfg.mqttSslEnable ? "TLS" : "TCP", g_cfg.mqttClientId);
  bool okConn = g_cfg.mqttUser[0]
                    ? g_mqtt.connect(g_cfg.mqttClientId, g_cfg.mqttUser, g_cfg.mqttPass)
                    : g_mqtt.connect(g_cfg.mqttClientId);
  if (!okConn) {
    char tlsErr[100] = {0};
    if (g_cfg.mqttSslEnable) g_mqttNet.lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("[MQTT] connect failed, state=%d (%s), TLS: %s\n", g_mqtt.state(),
                  g_mqtt.state() == MQTT_CONNECT_BAD_CREDENTIALS ? "bad credentials" :
                  g_mqtt.state() == MQTT_CONNECT_UNAUTHORIZED   ? "unauthorized" :
                  g_mqtt.state() == MQTT_CONNECTION_TIMEOUT     ? "timeout / TLS" :
                  g_mqtt.state() == MQTT_CONNECT_FAILED         ? "network / TLS" : "other",
                  tlsErr[0] ? tlsErr : "-");
    return false;
  }
  const char* topics[] = {
    g_cfg.topicFlash, g_cfg.topicResetStm32, g_cfg.topicCheckStm32,
    g_cfg.topicResetEc600n, g_cfg.topicResetEsp32
  };
  for (const char* t : topics) {
    bool ok = g_mqtt.subscribe(t, 1);
    Serial.printf("[MQTT] subscribe %s %s\n", t, ok ? "OK" : "FAILED");
  }
  if (g_stm32LinkUp) {
    statusf("online %s ip=%s stm32=CONNECTED(0x%08lX) fw=%s rst=%d", g_cfg.deviceId,
            WiFi.localIP().toString().c_str(), (unsigned long)g_stm32DpId,
            FW_TAG, (int)esp_reset_reason());
  } else {
    statusf("online %s ip=%s stm32=DISCONNECTED fw=%s rst=%d", g_cfg.deviceId,
            WiFi.localIP().toString().c_str(),
            FW_TAG, (int)esp_reset_reason());
  }
  return true;
}

static void configureMqttClient() {
  if (g_cfg.mqttSslEnable) {
    if (g_cfg.mqttTlsInsecure) {
      g_mqttNet.setInsecure();
    } else {
      g_mqttNet.setCACert(MQTT_ROOT_CA);
    }
    g_mqttNet.setHandshakeTimeout(g_cfg.mqttTlsHandshakeSec);
    g_mqtt.setClient(g_mqttNet);
  } else {
    g_mqtt.setClient(g_mqttPlain);
  }
  g_mqtt.setServer(g_cfg.mqttHost, g_cfg.mqttPort);
  g_mqtt.setKeepAlive(g_cfg.mqttKeepalive);
  g_mqtt.setSocketTimeout(g_cfg.mqttTlsHandshakeSec);
  g_mqtt.setBufferSize(1024);
  g_mqtt.setCallback(onMqttMessage);
}

static uint8_t  g_mqttFailStreak = 0;
static uint8_t  g_wifiFailStreak = 0;

static void serviceMqtt(uint32_t now) {
  if (g_mqtt.connected()) {
    g_mqtt.loop();
    return;
  }
  uint32_t baseRetry = max<uint32_t>(g_cfg.mqttRetryMs, 5000UL);
  uint32_t backoffMs = baseRetry;
  if (g_mqttFailStreak >= 3)      backoffMs = 30000UL;
  else if (g_mqttFailStreak == 2) backoffMs = 20000UL;
  else if (g_mqttFailStreak == 1) backoffMs = 10000UL;

  if (g_mqttUp) {
    g_mqttUp = false;
    Serial.printf("[MQTT] disconnected (state=%d), retry in %lu ms\n",
                  g_mqtt.state(), (unsigned long)baseRetry);
    g_mqttNextAttempt = now + baseRetry;
    return;
  }
  if ((int32_t)(now - g_mqttNextAttempt) < 0) return;

  if (mqttConnect()) {
    g_mqttUp = true;
    g_mqttFailStreak = 0;
  } else {
    if (g_mqttFailStreak < 10) g_mqttFailStreak++;
    g_mqttNextAttempt = millis() + backoffMs;
    Serial.printf("[MQTT] retry in %lu ms (streak=%u)\n",
                  (unsigned long)backoffMs, (unsigned)g_mqttFailStreak);
  }
}

// ---------------------------------------------------------------------------
// WiFi STA

static void wifiBegin() {
  if (g_cfg.wifiSsid[0] == '\0') {
    g_wifiConnecting = false;
    g_wifiNextAttempt = millis() + max<uint32_t>(g_cfg.wifiRetryMs, 3000UL);
    return;
  }
  Serial.printf("[WiFi] connecting to \"%s\" ...\n", g_cfg.wifiSsid);
  g_wifiConnecting = true;
  g_lastWifiAttempt = millis();
  const char* pass = (g_cfg.wifiPass[0] != '\0') ? g_cfg.wifiPass : nullptr;
  if (WiFi.begin(g_cfg.wifiSsid, pass) == WL_CONNECT_FAILED) {
    g_wifiConnecting = false;
    g_wifiNextAttempt = millis() + max<uint32_t>(g_cfg.wifiRetryMs, 3000UL);
  }
}

static void onWifiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_wifiConnecting = false;
    g_wifiFailStreak = 0;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_wifiConnecting = false;
    if (g_wifiFailStreak < 20) g_wifiFailStreak++;
    uint32_t baseWait = max<uint32_t>(g_cfg.wifiRetryMs, 3000UL);
    uint32_t waitMs = WiFi.softAPgetStationNum() > 0
                          ? max<uint32_t>(baseWait, 5000UL)
                          : baseWait;
    g_wifiNextAttempt = millis() + waitMs;
  }
}

static void startSoftAp();

static void serviceWifi(uint32_t now) {
  const bool up = WiFi.status() == WL_CONNECTED;
  uint32_t effTimeout = max<uint32_t>(g_cfg.wifiTimeoutMs, 15000UL);
  uint32_t effRetry   = max<uint32_t>(g_cfg.wifiRetryMs, 3000UL);

  if (up && !g_wifiUp) {
    Serial.printf("[WiFi] connected, IP %s RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    g_wifiConnecting = false;
    g_wifiFailStreak = 0;
    g_wifiPulseUntil = now + LED_WIFI_PULSE_MS;
    g_wifiUp = true;
    g_mqttNextAttempt = now;
  } else if (!up && g_wifiUp) {
    Serial.printf("[WiFi] connection lost, retry in %lu ms\n", (unsigned long)effRetry);
    g_wifiUp = false;
    g_mqttUp = false;
    g_mqttFailStreak = 0;
    g_mqtt.disconnect();
    g_wifiConnecting = false;
    g_wifiNextAttempt = now + effRetry;
  }
  if (!up) {
    if (g_wifiConnecting && (now - g_lastWifiAttempt >= effTimeout)) {
      g_wifiConnecting = false;
      if (g_wifiFailStreak < 20) g_wifiFailStreak++;
      WiFi.disconnect(false, false);
      if (g_wifiFailStreak >= 5) {
        /* Hard-cycle WiFi radio if STA state machine gets stuck across retries */
        Serial.println("[WiFi] Resetting radio state after 5 failed attempts");
        g_wifiFailStreak = 0;
        WiFi.disconnect(false, true);
        WiFi.mode(WIFI_OFF);
        delay(100);
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        startSoftAp();
      }
      g_wifiNextAttempt = now + effRetry;
    }
    if (!g_wifiConnecting && (int32_t)(now - g_wifiNextAttempt) >= 0) {
      wifiBegin();
    }
  }
}

static void startSoftAp() {
  WiFi.softAP(g_cfg.apSsid, strlen(g_cfg.apPass) >= 8 ? g_cfg.apPass : nullptr);
  Serial.printf("[AP] SSID=\"%s\" IP=%s\n", g_cfg.apSsid,
                WiFi.softAPIP().toString().c_str());
}

static void applyNetworkConfig(bool apChanged) {
  g_mqtt.disconnect();
  g_mqttUp = false;
  configureMqttClient();
  if (apChanged) startSoftAp();
  WiFi.disconnect(false, false);
  g_wifiUp = false;
  g_wifiConnecting = false;
  g_wifiNextAttempt = millis();
  g_mqttNextAttempt = millis();
  wifiBegin();
}

// ---------------------------------------------------------------------------
// EC600N reset control (PING_STM32 out) & alert monitor (STM32_STATUS in)

static void serviceStm32ControlPins(uint32_t now) {
#if PING_STM32 >= 0
  if (g_pingPulseUntil != 0 && (int32_t)(now - g_pingPulseUntil) >= 0) {
    g_pingPulseUntil = 0;
    digitalWrite(PING_STM32, LOW);
    statusf("reset ec600n: pulse done (PING_STM32 LOW)");
  }
#endif

#if STM32_STATUS >= 0
  static bool     inited = false;
  static int      lastRaw = LOW;
  static int      stableLvl = LOW;
  static uint32_t lastChangeMs = 0;

  int raw = digitalRead(STM32_STATUS);
  if (!inited) {
    inited = true;
    lastRaw = raw;
    stableLvl = raw;
    lastChangeMs = now;
    g_stm32ResetNeeded = (stableLvl == HIGH);
    return;
  }
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeMs = now;
  }
  if (raw != stableLvl && (uint32_t)(now - lastChangeMs) >= PING_DEBOUNCE_MS) {
    stableLvl = raw;
    g_stm32ResetNeeded = (stableLvl == HIGH);
    if (g_stm32ResetNeeded) {
      statusf("stm32 alert: CAN RESET EC600N (STM32_STATUS GPIO%d HIGH)", STM32_STATUS);
    } else {
      statusf("stm32 alert: cleared (STM32_STATUS GPIO%d LOW)", STM32_STATUS);
    }
  }
#else
  (void)now;
#endif
}

// ---------------------------------------------------------------------------
// Serial debug commands (optional, via USB monitor 115200)

static void serviceSerial() {
  static char line[64];
  static size_t pos = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (pos + 1 < sizeof(line)) line[pos++] = c;
      continue;
    }
    line[pos] = 0;
    pos = 0;
    String cmd(line);
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0) continue;

    if (cmd == "check" || cmd == "c") {
      queueJob(JOB_CHECK_STM32, nullptr);
    } else if (cmd == "reset") {
      queueJob(JOB_RESET_STM32, nullptr);
    } else if (cmd == "ec" || cmd == "simreset") {
      triggerEc600nResetPulse();
    } else if (cmd == "status" || cmd == "s") {
      Serial.printf("[INFO] WiFi=%s IP=%s MQTT=%s STM32=%s DP=0x%08lX ec600n_alert=%s busy=%d\n",
                    g_wifiUp ? "UP" : "DOWN",
                    g_wifiUp ? WiFi.localIP().toString().c_str() : "-",
                    g_mqttUp ? "UP" : "DOWN",
                    g_stm32LinkUp ? "CONNECTED" : "DISCONNECTED",
                    (unsigned long)g_stm32DpId,
                    g_stm32ResetNeeded ? "HIGH" : "LOW", (int)g_busy);
    } else {
      Serial.println("[HELP] commands: check, status, reset, ec");
    }
  }
}

// ---------------------------------------------------------------------------

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, !LED_ON_LEVEL);       // LED D2 off at boot

  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("=== ESP32 -> STM32F401 remote SWD programmer (MQTTS + HTTP) ===");

#if STM32_NRST_PIN >= 0
  pinMode(STM32_NRST_PIN, INPUT);             // released (open-drain style)
#endif
#if PING_STM32 >= 0
  pinMode(PING_STM32, OUTPUT);
  digitalWrite(PING_STM32, LOW);
#endif
#if STM32_STATUS >= 0
  pinMode(STM32_STATUS, INPUT_PULLDOWN);
#endif
  swdRelease();                               // don't disturb target at boot

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed - downloads will fail");
  } else {
    Serial.printf("[FS] LittleFS %u/%u bytes used\n", (unsigned)LittleFS.usedBytes(),
                  (unsigned)LittleFS.totalBytes());
  }

  configLoad(&g_cfg);

  g_jobQueue = xQueueCreate(1, sizeof(Job));
  g_statusQueue = xQueueCreate(24, STATUS_LEN);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(workerTask, "flash", 16384, nullptr, 1, nullptr, 1);

  configureMqttClient();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true);  // clear stale NVS WiFi STA state on boot
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);          // disable modem sleep to prevent AP+STA drops
  WiFi.onEvent(onWifiEvent);
  startSoftAp();

  WebConfigHooks hooks = {
    []() -> bool { return g_wifiUp; },
    []() -> bool { return g_mqttUp; },
    []() -> bool { return g_stm32LinkUp; },
    []() -> uint32_t { return g_stm32DpId; },
    []() -> bool { return g_busy; },
    []() -> bool { return g_stm32ResetNeeded; },
    applyNetworkConfig,
    []() -> bool { return queueJob(JOB_CHECK_STM32, nullptr); },
    triggerEc600nResetPulse,
    []() {
      if (!g_busy) {
        g_restartAt = millis() + 300;
        if (g_restartAt == 0) g_restartAt = 1;
      }
    },
  };
  webConfigInit(&g_cfg, hooks);

  wifiBegin();
}

void loop() {
  const uint32_t now = millis();

  serviceWifi(now);
  if (g_wifiUp) serviceMqtt(now);
  publishPendingStatus();
  webConfigLoop();
  serviceStm32ControlPins(now);
  serviceSerial();

#if STM32_NRST_PIN >= 0
  if (g_stm32ResetUntil && (int32_t)(now - g_stm32ResetUntil) >= 0) {
    pinMode(STM32_NRST_PIN, INPUT);
    g_stm32ResetUntil = 0;
    Serial.println("[STM32] NRST pulse done");
  }
#endif

  if (g_restartAt && (int32_t)(now - g_restartAt) >= 0) {
    publishPendingStatus();
    g_mqtt.disconnect();
    delay(100);
    ESP.restart();
  }

  delay(5);
}
