\# 🔧 Hướng dẫn nạp firmware Black Magic Probe bằng GDB (không cần build)



Tài liệu này hướng dẫn cách sử dụng \*\*GDB + Black Magic Probe (BMP)\*\* để nạp firmware `.elf` trực tiếp cho STM32 (Bluepill - STM32F103), \*\*không cần dùng PlatformIO build\*\*.



\---



\## 📦 Yêu cầu



\* Black Magic Probe (hoặc Bluepill đang đóng vai BMP)

\* File firmware:



&#x20; \* `blackmagic\_dfu-bluepill.elf`

&#x20; \* `blackmagic-bluepill.elf`

\* GDB ARM:



&#x20; \* `arm-none-eabi-gdb` (có thể lấy từ PlatformIO)



\---



\## ⚙️ Chuẩn bị



\### 1. Xác định cổng COM của BMP



Ví dụ:



```bash

COM8

```



\---



\### 2. Mở GDB



```bash

arm-none-eabi-gdb

```



\---



\## 🚀 Quy trình nạp firmware



\### Bước 1: Kết nối tới Black Magic Probe



```gdb

target extended-remote COM8

monitor swdp\_scan

attach 1

```



\---



\### Bước 2: Dừng CPU và xóa flash



```gdb

monitor halt

monitor erase\_mass

```



\---



\### Bước 3: Nạp DFU bootloader



```gdb

file E:/Project\_Kicad/BMP\_Bluepill/blackmagic\_dfu-bluepill.elf

load

```



\---



\### Bước 4: Nạp firmware chính



```gdb

file E:/Project\_Kicad/BMP\_Bluepill/blackmagic-bluepill.elf

load

```



\---



\### Bước 5: Reset và chạy



```gdb

monitor reset

continue

```



\---



\## 📌 Lưu ý quan trọng



\### ❗ Luôn dùng đúng GDB



```bash

arm-none-eabi-gdb

```



Không dùng:



```bash

gdb

```



\---



\### ❗ Không truyền địa chỉ khi dùng `.elf`



Sai:



```gdb

load firmware.elf 0x08000000

```



Đúng:



```gdb

file firmware.elf

load

```



\---



\### ❗ Thứ tự nạp



1\. DFU (`blackmagic\_dfu`)

2\. Firmware chính (`blackmagic`)



\---



\### ❗ Nếu lỗi



Thử:



```gdb

monitor erase\_mass

```



hoặc:



```gdb

monitor unlock\_flash

```



\---



\## ⚡ Tự động hóa (khuyên dùng)



\### Tạo file `flash.gdb`



```gdb

target extended-remote COM8

monitor swdp\_scan

attach 1

monitor halt

monitor erase\_mass



file blackmagic\_dfu-bluepill.elf

load



file blackmagic-bluepill.elf

load



monitor reset

continue

quit

```



\---



\### Chạy lệnh:



```bash

arm-none-eabi-gdb -x flash.gdb

```



\---



\## 🧠 Ghi chú



\* File `.elf` đã chứa địa chỉ flash → không cần chỉ định thêm

\* STM32F103 có flash bắt đầu tại `0x08000000`

\* DFU bootloader thường chiếm 8KB đầu (`0x08000000 → 0x08002000`)



\---



\## ✅ Kết quả



Sau khi nạp thành công:



\* Board sẽ hoạt động như Black Magic Probe

\* Xuất hiện 2 cổng COM:



&#x20; \* GDB

&#x20; \* UART



\---



\## 🎯 Tổng kết



\* Không cần PlatformIO build

\* Không cần OpenOCD

\* Chỉ cần GDB + BMP

\* Nạp trực tiếp `.elf`



\---



\## 📞 Troubleshooting nhanh



| Lỗi                        | Nguyên nhân | Cách fix                 |

| -------------------------- | ----------- | ------------------------ |

| unknown architecture "arm" | Sai GDB     | dùng `arm-none-eabi-gdb` |

| load failed                | chưa erase  | `monitor erase\_mass`     |

| attach lỗi                 | sai kết nối | `monitor swdp\_scan`      |

| không thấy target          | dây SWD sai | kiểm tra SWDIO/SWCLK     |



\---



\## 🚀 Tip



Có thể tạo `.bat` để flash 1 click nếu dùng Windows.



