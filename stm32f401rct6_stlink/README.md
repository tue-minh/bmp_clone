# STM32F401RCT6 CMSIS-DAP Debug Probe & Multi-STM32 SWD Programmer (`TUE CMSIS-DAP`)

Dự án bao gồm:
* **[firmware/](firmware/):** Firmware cho **STM32F401RCT6** biến vi điều khiển thành mạch nạp và debug **ARM CMSIS-DAP v1.10 (USB Custom HID 64-byte)** + **USB VCP UART** kiêm bộ điều khiển/nạp **SWD đa dòng STM32 (F1, F3, F4, G4, L4, F0, G0...)** — file nạp sẵn nằm ngay tại [firmware/tue-cmsis-dap-stm32f401rct6.bin](firmware/tue-cmsis-dap-stm32f401rct6.bin) và `.hex`.
* **[gui/](gui/):** Ứng dụng giao diện C++17 native cho Windows ([gui/TUE_CMSIS_DAP_CPP.exe](gui/TUE_CMSIS_DAP_CPP.exe)) và Ubuntu/Linux ([gui/linux/tue-cmsis-dap_1.0.1_amd64.deb](gui/linux/tue-cmsis-dap_1.0.1_amd64.deb)).
* **[ESP_OTA_STM32/](ESP_OTA_STM32/):** Dự án gốc trên ESP32 nạp OTA qua giao thức SWD cho STM32F401 (nền tảng của tầng SWD ADIv5).

Xem tài liệu chi tiết về sơ đồ chân, cấu trúc code, hướng dẫn build và sử dụng với **OpenOCD / pyOCD / Keil MDK / STM32CubeIDE / VSCode** tại **[firmware/README.md](firmware/README.md)**.
