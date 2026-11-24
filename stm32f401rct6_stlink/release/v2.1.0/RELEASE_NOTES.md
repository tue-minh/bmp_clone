# TUE CMSIS-DAP v2.1.0 Release

## 1. Firmware STM32F401RCT6
- **File `.bin`:** `tue-cmsis-dap-stm32f401rct6-v2.1.0.bin` (`52,032 bytes`, nạp tại `0x08000000` qua DFU hoặc ST-Link)
- **File `.hex`:** `tue-cmsis-dap-stm32f401rct6-v2.1.0.hex` (`146,406 bytes`)
- **Tính năng:**
  - USB Composite: CMSIS-DAP v1.10 HID (`VID=0x0483, PID=0x5752`) + Virtual COM Port (`USART1` `PA9/PB7`).
  - LED `PC13` trạng thái công nghiệp (Heartbeat Idle, Solid Connected, Flicker Flashing, Double Blink Debug Halted, Triple Blink Error).
  - Nút bấm `PA0`: Target Hardware Reset (`nRESET` `PB8`) + Probe link.

## 2. Ứng dụng Windows (Native C++17 Win32)
- **File:** `TUE_CMSIS_DAP_Windows_v2.1.0.exe` (`1,408,512 bytes`, chạy độc lập không cần cài đặt).
- **Tính năng:**
  - Tự động nhận diện USB HID & COM Port (Plug & Play).
  - Nạp/Verify/Mass Erase trực tiếp cho STM32F1/F3/F4/G4/L4 không cần OpenOCD, hoặc tích hợp OpenOCD + GDB Server.
  - Đọc trực tiếp thông tin chip & 256 bytes Flash đầu tiên khi nhấn `Connect`.
  - 9 mức tốc độ SWD (`4 MHz` – `20 kHz`), Memory Hex Viewer & Mem32, UART Terminal (`Echo TX`, `\r`, `\n`, `\t`, `Parse \r \n \t \xHH`).

## 3. Ứng dụng Ubuntu / Linux (Native C++17 Qt5)
- **Gói cài đặt `.deb`:** `tue-cmsis-dap_2.1.0_amd64.deb` (`875,566 bytes`, tự động cài `/usr/local/bin/tue-cmsis-dap`, icon logo, shortcut & `udev rules`).
  ```bash
  sudo apt install ./tue-cmsis-dap_2.1.0_amd64.deb
  ```
- **File chạy độc lập:** `tue-cmsis-dap-ubuntu-x86_64` (`252,296 bytes`).
