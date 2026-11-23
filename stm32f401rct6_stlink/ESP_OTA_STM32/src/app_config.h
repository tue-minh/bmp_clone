#pragma once

#include <Arduino.h>

struct AppConfig {
  // WiFi STA
  char     wifiSsid[64];
  char     wifiPass[64];
  uint32_t wifiRetryMs;
  uint32_t wifiTimeoutMs;

  // SoftAP (192.168.4.1 setup portal)
  char     apSsid[32];
  char     apPass[64];

  // MQTTS / MQTT
  char     mqttHost[96];
  uint16_t mqttPort;
  bool     mqttSslEnable;
  bool     mqttTlsInsecure;
  uint16_t mqttTlsHandshakeSec;
  char     deviceId[48];
  char     mqttClientId[64];
  char     mqttUser[48];
  char     mqttPass[64];
  uint16_t mqttKeepalive;
  uint32_t mqttRetryMs;

  // MQTT topics
  char     topicFlash[64];
  char     topicResetStm32[64];
  char     topicCheckStm32[64];
  char     topicResetEc600n[64];
  char     topicResetEsp32[64];
  char     topicStatus[64];

  // HTTP / HTTPS firmware download
  char     defaultFwUrl[256];
  uint32_t httpTimeoutMs;
  bool     httpsInsecure;
};

struct WebConfigHooks {
  bool     (*isWifiUp)();
  bool     (*isMqttUp)();
  bool     (*isStm32Up)();
  uint32_t (*getStm32Dp)();
  bool     (*isBusy)();
  bool     (*isStm32ResetNeeded)();
  void     (*onApply)(bool apChanged);
  bool     (*onCheckStm32)();
  void     (*onResetEc600n)();
  void     (*onReboot)();
};

void configDefaults(AppConfig* cfg);
void configLoad(AppConfig* cfg);
bool configSave(const AppConfig* cfg);
void configResetDefaults(AppConfig* cfg);

void webConfigInit(AppConfig* cfg, const WebConfigHooks& hooks);
void webConfigLoop();
