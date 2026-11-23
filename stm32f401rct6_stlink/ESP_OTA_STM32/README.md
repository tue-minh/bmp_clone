# ESP32 SWD Remote Programmer & EC600N Control Bridge (`ESP_OTA_STM32`)

Firmware cho module **ESP32 (`esp32dev`, PlatformIO Arduino)** thực hiện:
1. **Nạp firmware `.bin` từ xa cho STM32F401 qua giao thức SWD** (nhận lệnh MQTTS, tải file HTTP/HTTPS lưu vào LittleFS, nạp vào Flash `0x08000000` và verify).
2. **Giám sát & điều khiển reset EC600N / STM32 / ESP32** thông qua MQTT, Web Portal cục bộ và Serial CLI.
3. **Web Portal cấu hình lưu EEPROM** (`WiFi`, `SoftAP`, `MQTTS`, `MQTT Topics`, `HTTP OTA`) luôn khả dụng tại `192.168.4.1` (SoftAP) và IP STA.

---

## 1. Sơ đồ kết nối chân (`ESP32` ↔ `STM32F401`)

Cấu hình chi tiết trong [src/config.h](src/config.h):

| Chân ESP32 | Vai trò trong code | Kết nối STM32F401 | Mô tả hoạt động |
|---|---|---|---|
| **GPIO18** | `SWD_PIN_SWDIO` | `PA13` (`SWDIO`) | Đường dữ liệu SWD nạp firmware & đọc DP ID (`0x423` / Cortex-M4) |
| **GPIO19** | `SWD_PIN_SWCLK` | `PA14` (`SWCLK`) | Xung nhịp SWD |
| **GPIO21** | `STM32_NRST_PIN` | `NRST` | Mặc định thả nổi (`INPUT`); kéo `LOW` `50ms` để reset cứng STM32 |
| **GPIO26** | `PING_STM32` (Output) | `PA5` (`ESP32_STATUS`, `EXTI9_5`) | Mặc định `LOW`; kéo **`HIGH` 1 lần trong `300ms`** (`PING_EC_RESET_PULSE_MS`) để ra lệnh cho STM32 reset EC600N |
| **GPIO25** | `STM32_STATUS` (`INPUT_PULLDOWN`) | `PA4` (`PING_ESP32`) | Đọc trạng thái với debounce `50ms`: `HIGH` = STM32 báo EC600N retry `30s` lần 1 thất bại (cần reset), `LOW` = bình thường |
| **EN** | Reset cứng ESP32 | `PA6` (`ESP32_RESET`, Open-Drain) | Dự phòng điều khiển từ phía STM32 (mặc định `PA6` giữ `HIGH`, không tự reset) |
| **GPIO2** | `LED_PIN` (LED onboard) | — | Tắt = mất WiFi; sáng `2.5s` = vừa có WiFi; sáng liên tục = có WiFi chưa có MQTT; nháy chậm `700ms` = WiFi + MQTT OK |

---

## 2. MQTT Topics & Điều khiển từ xa

Broker mặc định: **`m12dfadd.ala.asia-southeast1.emqxsl.com:8883`** (TLS 1.2 với `DigiCert Global Root G2`). ESP32 tự động retry WiFi và MQTTS mỗi **`1 giây`** khi mất kết nối.

| Topic | Hướng | Chức năng | Payload / Ghi chú |
|---|---|---|---|
| `esp32/to_stm32/flash` | Subscribe | Tải `.bin` và nạp vào STM32 qua SWD | URL chuỗi thuần hoặc JSON `{"url": "http://.../firmware.bin"}` (rỗng = dùng `defaultFwUrl`) |
| `esp32/to_stm32/reset/stm32` | Subscribe | Reset STM32 | Kéo `NRST` (`GPIO21`) + reset SWD |
| `esp32/to_stm32/check` | Subscribe | Kiểm tra kết nối SWD tới STM32 | Đọc `DP ID` & trạng thái target, trả kết quả lên `esp32/status` |
| `esp32/to_stm32/reset/ec600n` | Subscribe | Ra lệnh STM32 reset modem 4G EC600N | ESP32 kéo `GPIO26` lên `HIGH` `300ms` (STM32 bắt sườn lên trên `PA5` và kéo `PA8` reset EC600N `250ms`) |
| `stm32/to_esp32/reset` | Subscribe | Khởi động lại ESP32 | Gọi `ESP.restart()` |
| `esp32/status` | Publish | Gửi log trạng thái & cảnh báo | Báo tiến độ OTA, lỗi SWD, IP, và thông báo khi `GPIO25` chuyển `HIGH`/`LOW` |

---

## 3. Web Cấu hình & EEPROM

Firmware mở **WebServer (port 80)** trên cả:
- **SoftAP:** `ESP32-SWD-Setup` (mật khẩu mặc định `12345678`, truy cập `http://192.168.4.1`)
- **WiFi STA:** truy cập qua IP mà router cấp (xem trên Serial hoặc `esp32/status`)

Các endpoint hỗ trợ ([src/app_config.cpp](src/app_config.cpp)):
- `GET /`: Hiển thị trạng thái WiFi, MQTT, SWD STM32, trạng thái **`EC600N (GPIO25)`** (`CAN RESET EC600N (PA4 HIGH)` hoặc `Binh thuong (LOW)`) và form cấu hình toàn bộ thông số.
- `POST /save`: Lưu cấu hình vào EEPROM (`kEepromVersion = 3`) và áp dụng ngay.
- `POST /check`: Kiểm tra SWD STM32.
- `POST /reset-ec600n`: Gửi xung `HIGH 300ms` trên `GPIO26` để reset EC600N.
- `POST /reboot`: Khởi động lại ESP32.
- `POST /reset-cfg`: Khôi phục cấu hình mặc định trong [src/config.h](src/config.h) và lưu EEPROM.

---

## 4. Lệnh Debug qua Serial (`115200 baud`)

| Lệnh | Tác dụng |
|---|---|
| `check` / `c` | Kiểm tra kết nối SWD tới STM32 |
| `status` / `s` | In trạng thái WiFi, IP, MQTT, STM32 SWD, `ec600n_alert` (`GPIO25`), `busy` |
| `reset` | Reset STM32 qua `NRST`/SWD |
| `ec` / `simreset` | Kéo xung `HIGH 300ms` trên `GPIO26` để kích reset EC600N |

---

## 5. Biên dịch, Nạp code & Công cụ Test

### 5.1 Build & Upload với PlatformIO
Mở terminal tại thư mục `ESP_OTA_STM32`:
```powershell
# Biên dịch
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run

# Nạp qua cổng COM (ví dụ COM8)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload --upload-port COM8

# Xem Serial Monitor
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor -b 115200
```

### 5.2 Server test cục bộ (`test_web/`)
Dùng [test_web/server.py](test_web/server.py) để host file `.bin` qua HTTP LAN, gửi lệnh MQTTS và theo dõi `esp32/status` trực tiếp trên trình duyệt:
```powershell
cd test_web
python server.py 8080
```
Mở trình duyệt tại `http://localhost:8080`.
