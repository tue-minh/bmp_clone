esp32/to_stm32/flash để báo nạp code

esp32/to_stm32/reset/stm32 để reset stm32

esp32/to_stm32/reset/ec600n để reset sim

stm32/to_esp32/reset để reset esp32

#define ESP32_MQTT_CLIENT_IDX          1       /* Client index 1 */
#define ESP32_MQTT_BROKER_HOST         "m12dfadd.ala.asia-southeast1.emqxsl.com"
#define ESP32_MQTT_BROKER_PORT         8883    /* Secure MQTTS port */
#define ESP32_DEVICE_ID                "esp32_hvnn_test_01"
#define ESP32_MQTT_CLIENT_ID           "esp32_" ESP32_DEVICE_ID
#define ESP32_MQTT_USERNAME            "esp32"
#define EC600N_MQTT_PASSWORD            "1234"
#define EC600N_MQTT_KEEPALIVE_SEC       60      /* Keepalive interval 60s */
#define EC600N_MQTT_VERSION             4       /* 4 = MQTT 3.1.1 */
#define EC600N_MQTT_SSL_ENABLE          1       /* 1 = Enable SSL for MQTT client */