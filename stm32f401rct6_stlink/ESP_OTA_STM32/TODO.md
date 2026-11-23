# ESP32 → STM32 Remote Firmware Upload over SWD (HTTP + MQTTS) — TODO

**Goal:** Build a fresh ESP32-WROOM-32D remote SWD programmer for the STM32. **No web server, no AP mode, and no web UI** — the ESP32 only connects to an existing WiFi network, communicates over **MQTTS**, downloads the `.bin` firmware over **HTTP/HTTPS**, flashes the STM32 over **SWD**, and reports status/debug via onboard **LED D2**.

**Target:** STM32F401RCT6 — flash base `0x08000000`, 256 KB, DEV_ID `0x423`, F4 sector layout (4 × 16K, 1 × 64K, 3 × 128K), single-bank F4 flash controller (`FLASH_R_BASE 0x40023C00`).

| ESP32 pin | Target / Peripheral | Function |
|-----------|---------------------|----------|
| GPIO18 (D18) | STM32 SWDIO (PA13) | SWD data (bidirectional) |
| GPIO19 (D19) | STM32 SWCLK (PA14) | SWD clock |
| GND | STM32 GND | Common ground (**required**) |
| *(optional)* GPIO21 | STM32 NRST | Hardware reset |
| 3V3 | STM32 3V3 | Only if the STM32 board is not powered separately |
| GPIO2 (D2) | Onboard LED D2 | WiFi & MQTT status / debug indicator |
| *(planned)* GPIO *(TBD)* | EC600N RST | Hardware reset for SIM module (`esp32/to_stm32/reset/ec600n`) |

> **Implementation note:** Arduino-ESP32 bundles `LittleFS`, `HTTPClient`,
> `WiFiClientSecure`, and ESP-IDF's `esp_mqtt_client` (or you can add `PubSubClient`
> in `platformio.ini`). `WebServer`, `ESPmDNS`, and `data/index.html` are not used.

---

## Phase 0 — Hardware setup
- [ ] Confirm target chip details: **STM32F401RCT6**, flash base `0x08000000`, 256 KB, F4 sectors (16K/16K/16K/16K/64K/128K…), F4 flash controller.
- [ ] Wire SWD lines as in the table above; keep wires short (under 15 cm).
- [ ] Add a 10k–100k pull-up resistor on SWDIO if the line is unreliable (also enable internal pull-up in software).
- [ ] Verify both boards run at 3.3 V (**never apply 5 V to signal lines**).
- [ ] (Optional) Wire `NRST` to `GPIO21` for hardware reset / connect-under-reset (otherwise reset via `AIRCR/SYSRESETREQ`).
- [ ] Verify onboard **LED D2 (`GPIO2`)** works properly on the ESP32 board.
- [x] Assign an ESP32 GPIO for `EC600N RST` if using `esp32/to_stm32/reset/ec600n`. → `EC600N_RST_PIN 22` in `config.h`.

## Phase 1 — Fresh project setup (`platformio.ini` & `src/`)
- [x] Configure `platformio.ini`: `espressif32`, `esp32dev`, `arduino`, `monitor_speed = 115200`, `board_build.filesystem = littlefs`.
- [x] Remove any unused web server / `data/index.html` setup and start fresh with clean modules:
  - `src/config.h` (WiFi STA credentials, MQTT config from [`mqtt.md`](mqtt.md), GPIO pins, HTTP/flash limits)
  - `src/main.cpp` (WiFi STA, MQTTS, HTTP downloader, LED D2 state machine, FreeRTOS flash queue)
  - `src/swd.cpp/.h` (low-level SWD bit-bang driver)
  - `src/stm32_flash.cpp/.h` (STM32 core control & F4 flash programmer)

## Phase 2 — Status & Debug LED D2 (`GPIO2`)
- [x] Configure `GPIO2` (`LED D2`) as `OUTPUT`, default **OFF** at boot.
- [x] Implement a non-blocking LED D2 controller (`millis()` or dedicated FreeRTOS task) with exact states:
  | Network / MQTT state | LED D2 behaviour |
  |---|---|
  | **WiFi disconnected / failed** | **OFF** (tắt đèn) |
  | **WiFi connected successfully** | **Solid ON quickly for 2–3 s, then off** (sáng liên tục nhanh trong 2–3s rồi thôi, chuyển sang báo MQTT) |
  | **WiFi OK, MQTT disconnected / failed** | **Solid ON** (sáng liên tục) |
  | **WiFi OK, MQTT connected** | **Slow blink** (nháy chậm, e.g. 500 ms–1 s toggle) |
- [x] Use LED D2 + Serial logs for runtime debugging without needing any web interface.

## Phase 3 — WiFi STA (existing network only)
- [x] Connect ESP32 in **WiFi STA mode only** to the existing WiFi network configured in `config.h` (no AP mode, no web server).
- [x] Trigger the LED D2 2–3 s solid-ON pulse whenever WiFi transitions to connected.
- [x] Auto-reconnect if the WiFi connection drops, turning LED D2 OFF while disconnected.

## Phase 4 — MQTTS connection & topics ([`mqtt.md`](mqtt.md))
- [x] Put MQTTS configuration in `config.h` matching [`mqtt.md`](mqtt.md):
  - `ESP32_MQTT_CLIENT_IDX`: `1`
  - `ESP32_MQTT_BROKER_HOST`: `"m12dfadd.ala.asia-southeast1.emqxsl.com"`
  - `ESP32_MQTT_BROKER_PORT`: `8883` (`EC600N_MQTT_SSL_ENABLE = 1`)
  - `ESP32_DEVICE_ID`: `"esp32_hvnn_test_01"`
  - `ESP32_MQTT_CLIENT_ID`: `"esp32_" ESP32_DEVICE_ID` (`"esp32_esp32_hvnn_test_01"`)
  - `ESP32_MQTT_USERNAME`: `"esp32"`
  - `EC600N_MQTT_PASSWORD`: `"1234"`
  - `EC600N_MQTT_KEEPALIVE_SEC`: `60`
  - `EC600N_MQTT_VERSION`: `4` (MQTT 3.1.1)
- [x] Establish TLS/SSL MQTTS connection to port `8883` once WiFi STA is connected, with non-blocking reconnect backoff.
- [x] Update LED D2 state automatically on MQTT connect (**slow blink**) and disconnect/failure (**solid ON**).
- [x] Subscribe and handle all 4 topics from [`mqtt.md`](mqtt.md):
  - [x] `esp32/to_stm32/flash` — Trigger remote STM32 firmware update (parse `.bin` URL from payload or use configured URL, download via HTTP/HTTPS, and queue SWD flash).
  - [x] `esp32/to_stm32/reset/stm32` — Reset the STM32 over SWD (`AIRCR/SYSRESETREQ`) or `NRST`.
  - [x] `esp32/to_stm32/reset/ec600n` — Pulse the EC600N reset GPIO to reset the SIM module.
  - [x] `stm32/to_esp32/reset` — Restart the ESP32 (`ESP.restart()`).

## Phase 5 — HTTP/HTTPS `.bin` firmware download
- [x] Implement `.bin` download using `HTTPClient` (`WiFiClient` / `WiFiClientSecure`) when triggered by `esp32/to_stm32/flash`.
- [x] Stream the firmware directly into LittleFS (`/firmware.bin`) without holding the full binary in RAM.
- [x] Validate HTTP status (`200 OK`), non-empty payload, and `size <= 256 KB` (target flash size) before queuing the flash task.
- [x] Guard against starting a new download/flash while an existing flash job is in progress.

## Phase 6 — Low-level SWD driver (`swd.cpp/.h`)
- [x] Define `SWDIO = 18`, `SWCLK = 19` in `config.h`.
- [x] Bit-bang SWD with direct GPIO registers (`GPIO.out_w1ts/w1tc`, `GPIO.enable_w1ts/w1tc`, `GPIO.in`).
- [x] Implement `swdLineReset()` (60 clocks high + 2 idle) and `swdJtagToSwd()` (`0xE79E`).
- [x] Implement `swdReadDP`/`WriteDP` and `swdReadAP`/`WriteAP` with request parity, turnaround, ACK, 32 data bits + parity.
- [x] Retry on `WAIT` (deadline-based) and clear sticky errors on `FAULT` via `ABORT = 0x1E`.
- [x] Read **DP IDCODE**. ✅ **Milestone 1**
- [x] Implement `swdPowerUp()` (`CTRL/STAT = 0x50000000`, wait for both power-up ACKs) and MEM-AP setup (`CSW = 0x23000012`, 32-bit auto-increment).
- [x] Implement `memRead32` / `memWrite32` via `TAR`/`DRW` (AP reads through `RDBUFF`).

## Phase 7 — STM32 core control & flash programming (`stm32_flash.cpp/.h`)
- [x] Halt core (`DHCSR = 0xA05F0003`) and read `DBGMCU_IDCODE` (`0xE0042000`) to confirm STM32F401 (`0x423`). ✅ **Milestone 2**
- [x] Freeze IWDG/WWDG via `DBGMCU_APB1_FZ` while held.
- [x] Unlock F4 flash (`0x45670123`, `0xCDEF89AB`) and re-lock (`LOCK` bit) after flashing.
- [x] Implement `flashWaitReady()` with `BSY` polling, error-bit decoding, and timeout.
- [x] Implement F4 sector erase (`SER` + `SNB` + `PSIZE=x32`, then `STRT` per RM0090) covering only the sectors needed for the `.bin` size.
- [x] Program flash from LittleFS `/firmware.bin` with `PG` + 32-bit words (tail padded with `0xFF`).
- [x] Verify written flash word-by-word against `/firmware.bin`.
- [x] Reset and run STM32 (`AIRCR = 0x05FA0004`, `DHCSR = 0xA05F0001`) after successful verify. ✅ **Milestone 3**
- [x] Run the entire flash sequence inside a dedicated FreeRTOS task fed by a queue (never inside the MQTT/WiFi callback).

## Phase 8 — Robustness & error handling
- [x] Detect STM32 Read-Out Protection (RDP) and report a clear error.
- [x] Keep all long loops watchdog-safe (`vTaskDelay` in the flash/download tasks).
- [x] Handle and log clear failures: WiFi drop, MQTTS auth/TLS failure, HTTP download error, no SWD target, power-up failure, verify mismatch, timeout, oversize `.bin`.

## Phase 9 — Hardware & end-to-end testing
- [ ] Run local PC test web tool (`python test_web/server.py` → open `http://127.0.0.1:8080/`) to host `.bin` over HTTP (`http://<PC_LAN_IP>:8080/firmware.bin`) and send MQTT test commands without adding any web server to the ESP32.
- [ ] Verify **LED D2** states:
  - [ ] WiFi failed/disconnected → LED OFF
  - [ ] WiFi connected → LED solid ON for 2–3 s, then off
  - [ ] WiFi OK + MQTT disconnected → LED solid ON
  - [ ] WiFi OK + MQTT connected → LED slow blink
- [ ] Verify SWD `DP IDCODE` and `DBGMCU_IDCODE` read 100/100 times reliably.
- [ ] Verify MQTTS connection to `m12dfadd.ala.asia-southeast1.emqxsl.com:8883` and auto-reconnect after WiFi/broker drops.
- [ ] End-to-end remote OTA: publish to `esp32/to_stm32/flash` → ESP32 downloads `.bin` via HTTP → flashes STM32 over SWD → verifies & resets STM32; test switching between blink A and blink B binaries and a large binary close to 256 KB.
- [ ] Unplug SWD mid-flash and confirm clean error recovery without crashing ESP32.
- [ ] Verify remote reset topics: `esp32/to_stm32/reset/stm32`, `esp32/to_stm32/reset/ec600n`, and `stm32/to_esp32/reset`.

---

### References
- ARM Debug Interface v5 (ADIv5) specification, SWD protocol
- STM32F4 Reference Manual **RM0090** (Flash section)
- Project MQTT configuration: [`mqtt.md`](mqtt.md)
