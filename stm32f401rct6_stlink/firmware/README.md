# STM32F401RCT6 CMSIS-DAP Debug Probe & Multi-STM32 SWD Programmer (`TUE CMSIS-DAP`)

Firmware biến vi điều khiển **STM32F401RCT6** thành mạch nạp và debug **CMSIS-DAP v1.10 (USB Custom HID 64-byte)** kết hợp bộ điều khiển/nạp **SWD đa dòng STM32 (F1, F3, F4, G4, L4, F0, G0...)** được phát triển và mở rộng từ nền tảng SWD của dự án `ESP_OTA_STM32`.

---

## 1. Tính năng chính

1. **Mạch nạp & Debug chuẩn ARM CMSIS-DAP v1.10 qua USB (`TUE CMSIS-DAP`):**
   * Cắm USB (`PA11`/`PA12`) vào PC là nhận ngay thiết bị **USB HID (`VID=0x0483`, `PID=0x5750`, Product=`"TUE CMSIS-DAP"`)** không cần cài driver (Driverless trên Windows / Linux / macOS).
   * Tương thích trực tiếp với **OpenOCD**, **pyOCD**, **Probe-rs**, **Keil MDK**, **STM32CubeIDE**, **VSCode (Cortex-Debug)** để **nạp code + đặt breakpoint / step debug** cho mọi dòng ARM Cortex-M (**STM32F0, F1, F2, F3, F4, F7, G0, G4, L0, L4, H7**, GD32, nRF52, RP2040...).
2. **Driver SWD thanh ghi tốc độ cao ([Core/Src/swd.c](Core/Src/swd.c)):**
   * Được port từ `ESP_OTA_STM32/src/swd.cpp` sang điều khiển trực tiếp thanh ghi `GPIOx->BSRR`, `IDR`, `MODER` của STM32F401.
   * Định thời chính xác bằng bộ đếm chu kỳ CPU **`DWT->CYCCNT`** (hỗ trợ thay đổi xung nhịp `DAP_SWJ_Clock`, cấu hình turnaround `1..4` chu kỳ, `idle_cycles`, `wait_retry`).
3. **Bộ nhận diện & nạp Flash đa dòng STM32 độc lập ([Core/Src/stm32_target.c](Core/Src/stm32_target.c)):**
   * Khắc phục giới hạn chỉ nạp được `STM32F401` (`0x423`) của bản ESP32, tự động đọc `DBGMCU_IDCODE` (`0xE0042000` và `0x40015800`), nhận diện kích thước Flash, mức bảo vệ RDP và chọn đúng thuật toán Flash Controller cho từng họ:
     * **STM32F1 / STM32F3 (`0x40022000`):** Xóa theo Page (`1 KB` / `2 KB`), ghi **16-bit half-word**.
     * **STM32F2 / STM32F4 (`0x40023C00`):** Xóa theo Sector (`16 KB` × 4, `64 KB` × 1, `128 KB` × N), ghi **32-bit word (`PSIZE_X32`)**.
     * **STM32G4 / STM32L4 (`0x40022000`):** Xóa theo Page (`2 KB`), ghi **64-bit double-word**.
   * Tự động fallback sang **Connect-Under-Reset** qua chân `PB8` (`MCU_SWDRESET`) khi chip đích đang ngủ hoặc khóa chân SWD.
4. **CLI chẩn đoán qua `USART1` (`115200 8N1`) & Nút nhấn `PA0`:**
   * Nhấn nút `PA0` (`MCU_BUTTON`) hoặc gõ lệnh `probe` trên `USART1` để kiểm tra nóng chip đích đang cắm vào cổng SWD.

---

## 2. Sơ đồ chân phần cứng (`STM32F401RCT6`)

Cấu hình trong [firmware.ioc](firmware.ioc) và [Core/Inc/main.h](Core/Inc/main.h):

| Chân STM32F401 | Nhãn (`main.h`) | Chế độ GPIO / Ngoại vi | Kết nối / Chức năng |
|---|---|---|---|
| **`PA4`** | `MCU_SWDIO` | Output PP / Input + `GPIO_PULLUP` (`VERY_HIGH`) | Nối vào chân **`SWDIO` (`PA13`)** của chip đích (bán song công, đảo chiều bằng `GPIOA->MODER`) |
| **`PB4`** | `MCU_SWDCLK` | Output PP (`VERY_HIGH`) | Nối vào chân **`SWCLK` (`PA14`)** của chip đích |
| **`PB8`** | `MCU_SWDRESET` | Output Open-Drain (`GPIO_PIN_SET` mặc định) | Nối vào chân **`NRST`** của chip đích (kéo `LOW` khi reset / connect-under-reset) |
| **`PA11`** | `USB_OTG_FS_DM` | `AF10_OTG_FS` | Đường dữ liệu USB `D-` nối PC |
| **`PA12`** | `USB_OTG_FS_DP` | `AF10_OTG_FS` | Đường dữ liệu USB `D+` nối PC |
| **`PA9`** | `USART1_TX` | `AF7_USART1` (`115200 8N1`) | Xuất log & giao diện CLI chẩn đoán SWD |
| **`PB7`** | `USART1_RX` | `AF7_USART1` (`115200 8N1`) | Nhận lệnh CLI chẩn đoán SWD |
| **`PA0`** | `MCU_BUTTON` | Input + `GPIO_PULLUP` | Nhấn nút (kéo xuống `GND`) để tự động `probe` chip đích và in thông tin ra `USART1` |
| **`PC13`** | `MCU_LED` | Output PP | Đèn LED báo trạng thái kết nối CMSIS-DAP / hoạt động SWD |
| **`PH0` / `PH1`** | `RCC_OSC_IN/OUT` | HSE `25 MHz` | Thạch anh ngoài 25 MHz (`SYSCLK = 60 MHz`, `USB = 48 MHz`) |

---

## 3. Cấu trúc mã nguồn

```text
firmware/
├── Core/
│   ├── Inc/
│   │   ├── main.h            # Định nghĩa nhãn GPIO từ CubeMX
│   │   ├── swd.h             # API giao thức SWD ADIv5 & bit-level primitives
│   │   ├── dap.h             # API giao thức ARM CMSIS-DAP v1.10
│   │   └── stm32_target.h    # API nhận diện & nạp Flash đa dòng (F1/F3/F4/G4/L4)
│   └── Src/
│       ├── main.c            # Khởi tạo hệ thống, vòng lặp DAP_Task() + UART CLI + Button
│       ├── swd.c             # Driver SWD thanh ghi (PA4/PB4/PB8) + DWT timing
│       ├── dap.c             # Xử lý toàn bộ tập lệnh CMSIS-DAP v1.10
│       └── stm32_target.c    # Bảng DEV_ID & thuật toán Flash đa dòng STM32
├── USB_DEVICE/
│   ├── App/
│   │   ├── usbd_desc.c           # USB Descriptor ("TUE CMSIS-DAP")
│   │   └── usbd_custom_hid_if.c  # HID Report Descriptor 27-byte (64B IN/OUT) & callback
│   └── Target/
│       └── usbd_conf.h           # Cấu hình EP1 IN/OUT 64-byte, bInterval = 1 ms
└── CMakeLists.txt
```

---

## 4. Hướng dẫn Biên dịch (Build)

Dự án sử dụng **CMake** (chuẩn sinh từ STM32CubeMX):

```powershell
cd D:\Project_Kicad\bmp_clone\stm32f401rct6_stlink\firmware
cmake --preset Debug
cmake --build --preset Debug
```

File thực thi sau khi build nằm tại `build/Debug/firmware.elf` (có thể xuất ra `.bin` / `.hex` bằng `arm-none-eabi-objcopy -O binary build/Debug/firmware.elf build/Debug/firmware.bin`).

---

## 5. Hướng dẫn sử dụng nạp & Debug cho STM32F1, F4, G4...

Sau khi nạp firmware này vào con **STM32F401RCT6** và cắm cổng USB của nó vào máy tính, máy tính sẽ nhận thiết bị HID **`TUE CMSIS-DAP`**.

### 5.1. Dùng với OpenOCD (Nạp & Debug F1, F4, G4...)
Chỉ cần chọn interface `cmsis-dap.cfg` và target tương ứng với dòng chip bạn muốn nạp/debug:

* **Với STM32F1 (ví dụ STM32F103C8T6):**
  ```powershell
  openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg -c "program app_f1.elf verify reset exit"
  ```
* **Với STM32F4 (ví dụ STM32F401 / F407 / F411):**
  ```powershell
  openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg -c "program app_f4.elf verify reset exit"
  ```
* **Với STM32G4 (ví dụ STM32G431 / G474):**
  ```powershell
  openocd -f interface/cmsis-dap.cfg -f target/stm32g4x.cfg -c "program app_g4.elf verify reset exit"
  ```

### 5.2. Dùng với pyOCD / Probe-rs / Keil MDK / VSCode
* **pyOCD:**
  ```powershell
  pyocd list
  pyocd flash -t stm32g431cb firmware.bin
  pyocd gdbserver -t stm32f103rc
  ```
* **Keil MDK:** Vào *Options for Target -> Debug -> chọn **CMSIS-DAP Debugger** -> Settings* sẽ thấy **`TUE CMSIS-DAP`** và `SW Device` của chip đích.
* **STM32CubeIDE / VSCode Cortex-Debug:** Chọn Debug Probe là **OpenOCD (CMSIS-DAP)** hoặc **pyOCD**.

---

## 6. Giao diện CLI qua `USART1` (`PA9` TX / `PB7` RX, `115200 8N1`)

Khi chưa mở phiên debug trên PC, bạn có thể dùng Terminal (`115200 8N1`) nối vào `USART1` (hoặc nhấn nút `PA0`) để kiểm tra nhanh bất kỳ chip STM32 nào đang cắm vào dây SWD:

| Lệnh | Tác dụng |
|---|---|
| `probe` hoặc `c` | Kết nối SWD (tự động thử *connect-under-reset* nếu cần), đọc `DP_IDCODE`, `DBGMCU_IDCODE`, tên dòng chip (F1/F3/F4/G4/L4/F0/G0), dung lượng Flash, mức RDP rồi thả cho chip chạy tiếp |
| `link` | Kiểm tra nhanh đường truyền SWD (`DP_IDCODE`) mà không dừng CPU đích |
| `reset` hoặc `r` | Reset chip đích qua thanh ghi `AIRCR.SYSRESETREQ` + chân cứng `PB8` (`MCU_SWDRESET`) |
| `halt` | Dừng (Halt) CPU của chip đích |
| `run` | Cho CPU của chip đích chạy tiếp (Resume) |
| `read <0xADDR>` | Đọc 1 word 32-bit tại địa chỉ bất kỳ của chip đích (ví dụ: `read 0x08000000`, `read 0xE0042000`) |
| `status` hoặc `s` | Xem trạng thái kết nối USB CMSIS-DAP, xung HCLK và mức logic chân `NRST` |

---

## 7. Công cụ GUI Test trên Windows ([gui/tue_dap_gui.py](gui/tue_dap_gui.py))

Chạy thẳng bằng Python (không cần cài thêm thư viện ngoài cho phần USB HID):

```powershell
python gui\tue_dap_gui.py
```

GUI hỗ trợ 3 tab:
* **SWD & Target Probe:** Quét thiết bị HID, đọc `DAP_Info`, bật/tắt LED `PC13`, kích xung Reset `PB8`, tự nhận diện chip đích (F1/F3/F4/G4/L4/F0/G0...) và đọc bộ nhớ 32-bit.
* **Nạp Code & Debug (OpenOCD):** Tích hợp sẵn `openocd.exe` để test kết nối, nạp `.bin`/`.elf`/`.hex` và bật GDB Server (`:3333`).
* **UART CLI:** Kết nối `USART1` (`115200`) để gửi nhanh lệnh `probe`, `link`, `reset`, `halt`, `run`.

