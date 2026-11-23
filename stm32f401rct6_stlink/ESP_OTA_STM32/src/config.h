#pragma once

// ============================================================================
//  ESP32 -> STM32F401RCT6 remote SWD programmer — configuration
//  WiFi STA only, MQTTS commands, HTTP/HTTPS .bin download, SWD flashing.
// ============================================================================

// ---- WiFi (STA mode only, existing network) --------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID                 "VNUA-WIFI"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD             ""
#endif
#define WIFI_RETRY_INTERVAL_MS    3000    // base retry interval when WiFi disconnected
#define WIFI_CONNECT_TIMEOUT_MS   15000   // safety timeout for DHCP + AP_STA channel switch

// ---- SoftAP (always-available lightweight setup portal at 192.168.4.1) ----
#define AP_SSID                   "ESP32-SWD-Setup"
#define AP_PASSWORD               "12345678"   // >= 8 chars; "" for open AP

// ---- MQTTS (from mqtt.md) --------------------------------------------------
#define ESP32_MQTT_CLIENT_IDX     1       /* Client index 1 */
#define ESP32_MQTT_BROKER_HOST    "m12dfadd.ala.asia-southeast1.emqxsl.com"
#define ESP32_MQTT_BROKER_PORT    8883    /* Secure MQTTS port */
#define ESP32_DEVICE_ID           "esp32_hvnn_test_01"
#define ESP32_MQTT_CLIENT_ID      "esp32_" ESP32_DEVICE_ID
#define ESP32_MQTT_USERNAME       "ESP32"
#define EC600N_MQTT_PASSWORD      "esp32"
#define EC600N_MQTT_KEEPALIVE_SEC 60      /* Keepalive interval 60s */
#define EC600N_MQTT_VERSION       4       /* 4 = MQTT 3.1.1 */
#define EC600N_MQTT_SSL_ENABLE    1       /* 1 = Enable SSL for MQTT client */

#define MQTT_RETRY_INTERVAL_MS    5000    // base retry interval for MQTTS reconnect
#define MQTT_TLS_HANDSHAKE_SEC    10

// 1 = skip broker certificate validation (debug only). 0 = verify against
// MQTT_ROOT_CA below (DigiCert Global Root G2, used by EMQX Serverless).
#define MQTT_TLS_INSECURE         0

// ---- MQTT topics (from mqtt.md) --------------------------------------------
#define TOPIC_FLASH_STM32         "esp32/to_stm32/flash"        // nạp code STM32
#define TOPIC_RESET_STM32         "esp32/to_stm32/reset/stm32"  // reset STM32
#define TOPIC_RESET_EC600N        "esp32/to_stm32/reset/ec600n" // reset SIM
#define TOPIC_RESET_ESP32         "stm32/to_esp32/reset"        // reset ESP32
#define TOPIC_CHECK_STM32         "esp32/to_stm32/check"        // kiểm tra SWD STM32
// Progress / result messages published by the ESP32 (debug aid).
#define TOPIC_STATUS              "esp32/status"

// ---- HTTP/HTTPS firmware download ------------------------------------------
// Used when the flash payload is empty (or has no URL).
#define FIRMWARE_DEFAULT_URL      ""
#define FIRMWARE_PATH             "/firmware.bin"
#define HTTP_TIMEOUT_MS           15000
// 1 = accept any HTTPS server certificate for .bin downloads.
#define HTTPS_DOWNLOAD_INSECURE   1

// ---- GPIO ------------------------------------------------------------------
#define LED_PIN                   2       // onboard LED D2
#define LED_ON_LEVEL              HIGH
#define LED_WIFI_PULSE_MS         2500    // solid ON after WiFi connects
#define LED_GAP_MS                300     // short OFF gap before MQTT indication
#define LED_SLOW_BLINK_MS         700     // toggle period when MQTT connected

#define SWD_PIN_SWDIO             18      // -> STM32 PA13
#define SWD_PIN_SWCLK             19      // -> STM32 PA14
#define STM32_NRST_PIN            -1      // Reset via SWD AIRCR.SYSRESETREQ (no NRST wire)

// ---- STM32 <-> ESP32 Control Signals (no blink ping) -----------------------
#define PING_STM32                26      // ESP32 -> STM32 PA5: 1x HIGH pulse = reset EC600N
#define STM32_STATUS              25      // STM32 PA4 -> ESP32: HIGH = waiting EC600N reset
#define PING_EC_RESET_PULSE_MS    300     // HIGH pulse width sent on PING_STM32
#define PING_DEBOUNCE_MS          50      // debounce for STM32_STATUS level
#define STM32_NRST_PULSE_MS       50      // hardware NRST low pulse width

// ---- Target: STM32F401RCT6 -------------------------------------------------
#define STM32_FLASH_BASE          0x08000000UL
#define STM32_FLASH_SIZE          (256UL * 1024UL)
#define STM32_EXPECTED_DEV_ID     0x423

// ---- Broker root CA (DigiCert Global Root G2) ------------------------------
static const char MQTT_ROOT_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n";
