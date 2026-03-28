# 🚀 How to Load Firmware into STM32F103C8T6 (Bluepill)

Hướng dẫn chi tiết cách nạp firmware Blackmagic cho board Bluepill sử dụng ST-Link hoặc Black Magic Probe (BMP).

---

## 📂 1. Chuẩn bị File Firmware
Tìm các file sau trong thư mục build của bạn:
* `.../blackmagic-firmware-v1.10.2/bluepill/blackmagic_dfu-bluepill.elf`
* `.../blackmagic-firmware-v1.10.2/bluepill/blackmagic-bluepill.elf`

---

## 🛠️ 2. Cài đặt Công cụ (GDB)
Mở **PowerShell** hoặc **Terminal** và gõ lệnh:
`arm-none-eabi-gdb`

> [!TIP]
> **Nếu chưa cài đặt GDB:** Bạn có thể tận dụng GDB đi kèm với PlatformIO bằng cách chạy lệnh sau:
> ```powershell
> # Kiểm tra đường dẫn
> dir $env:USERPROFILE\.platformio\packages\toolchain-gccarmnoneeabi\bin
>
> # Chạy GDB trực tiếp
> & "$env:USERPROFILE\.platformio\packages\toolchain-gccarmnoneeabi\bin\arm-none-eabi-gdb.exe"
> ```

---

## ⚡ 3. Tiến hành Nạp Firmware

Tùy chọn thiết bị nạp của bạn:

### 🔹 Cách A: Sử dụng ST-LINK
Sử dụng công cụ `st-flash` để nạp trực tiếp qua giao thức SWD:
```bash
# Xóa bộ nhớ cũ
st-flash erase

# Nạp DFU Bootloader (tại địa chỉ 0x8000000)
st-flash --flash=0x20000 write .../blackmagic-firmware-v1.10.2/bluepill/blackmagic_dfu-bluepill.elf 0x8000000

# Nạp Firmware chính (tại địa chỉ 0x8002000) và Reset
st-flash --flash=0x20000 --reset write .../blackmagic-firmware-v1.10.2/bluepill/blackmagic-bluepill.elf 0x8002000

### 🔹 Cách B: Sử dụng Black Magic Probe (BMP)
Chạy GDB và thực hiện các lệnh sau:
# Kết nối tới BMP (Thay COMx bằng cổng tương ứng)
target extended-remote COMx           (Windows)
target extended-remote /dev/tty* (Linux)

monitor swdp_scan
attach 1
monitor halt
monitor erase_mass

# Nạp DFU Bootloader
file .../blackmagic-firmware-v1.10.2/bluepill/blackmagic_dfu-bluepill.elf
load

# Nạp Firmware chính
file .../blackmagic-firmware-v1.10.2/bluepill/blackmagic-bluepill.elf
load

monitor reset
continue

🧠 Ghi chú quan trọng

Địa chỉ Flash:
File .elf thường đã chứa sẵn thông tin địa chỉ nên đôi khi không cần chỉ định thủ công.

Vùng nhớ: STM32F103 có Flash bắt đầu tại 0x08000000.DFU Bootloader: Thường chiếm 8KB đầu tiên (0x08000000 → 0x08002000).✅ Kết quảSau khi nạp thành công, hãy rút board và cắm lại vào PC qua cổng USB. Nếu thành công, máy tính sẽ nhận 2 cổng Virtual COM:पोर्ट dành cho GDBपोर्ट dành cho UARTCongrats, and enjoy your new Blackmagic probe! 🎉📞 Troubleshooting nhanhLỗiNguyên nhânCách khắc phụcunknown architecture "arm"Sai phiên bản GDBSử dụng đúng arm-none-eabi-gdbload failedChưa xóa bộ nhớChạy monitor erase_mass trước khi loadattach lỗiSai kết nối vật lýKiểm tra lại lệnh monitor swdp_scankhông thấy targetDây SWD lỏng/saiKiểm tra kỹ chân SWDIO và SWCLK

