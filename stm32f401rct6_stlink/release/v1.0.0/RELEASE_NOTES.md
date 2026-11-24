# TUE CMSIS-DAP v1.0.0 Release

## 1. STM32F401RCT6 Firmware
- **`.bin`:** `tue-cmsis-dap-stm32f401rct6-v1.0.0.bin` (`45,296 bytes`, flash at `0x08000000` via DFU or ST-Link)
- **`.hex`:** `tue-cmsis-dap-stm32f401rct6-v1.0.0.hex`
- **Highlights:**
  - USB Composite: CMSIS-DAP v1.10 HID (`VID=0x0483, PID=0x5752`) + Virtual COM Port (`USART1` `PA9/PB7`).
  - Industrial status LED (`PC13`): Single Blink when idle/unconnected, Double Blink when connected to a target, Solid ON during Debug mode (with 3-pulse blink upon executing debug commands), and continuous rapid flicker during flashing/erasing/verifying/target actions until completed.
  - Direct non-blocking hold-to-reset button (`PA0` -> `PB8 nRESET` with internal pull-up).

## 2. Windows App (Native C++17 Win32)
- **Executable:** `TUE_CMSIS_DAP_Windows_v1.0.0.exe` (standalone, zero dependencies).
- **Highlights:**
  - Automatic USB Plug & Play detection (HID + COM).
  - Connect & Disconnect buttons, HW Reset (`nRESET` pin) & SWD Reset (`AIRCR.SYSRESETREQ`), direct `.bin/.hex/.elf` Flash/Verify/Mass Erase for STM32F1/F3/F4/G4/L4 (no OpenOCD needed) or optional `Use OpenOCD`.
  - Reads chip info + 512 bytes of on-chip Flash preview on Connect.
  - Dedicated **CPU Debug Window** (`Start Debug`, `Exit Debug`, `Halt`, `Run`, `Step Into`, `Step Out`, `Read Regs`), **Memory Hex Viewer & Mem32**, and **UART Terminal** (`Echo TX`, `\r`, `\n`, `\t`, `Parse \r \n \t \xHH`).

## 3. Ubuntu / Linux App (Native C++17 Qt5)
- **Debian Package (`.deb`):** `tue-cmsis-dap_1.0.0_amd64.deb` (installs `/usr/local/bin/tue-cmsis-dap`, app icon, desktop entry, and `udev rules`).
  ```bash
  sudo apt install ./tue-cmsis-dap_1.0.0_amd64.deb
  ```
- **Standalone Binary:** `tue-cmsis-dap-ubuntu-x86_64`.
