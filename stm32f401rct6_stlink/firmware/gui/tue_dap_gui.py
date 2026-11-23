"""
TUE CMSIS-DAP & Multi-STM32 SWD Programmer / Debugger GUI
- Direct USB HID (Windows hid.dll via ctypes, ZERO pip dependencies)
- Built-in Multi-STM32 Flasher & Verifier (STM32F1/F3, F2/F4, G4/L4) for .bin/.hex/.elf
  WITHOUT requiring OpenOCD or any external driver!
- Optional OpenOCD tab (for GDB server or extra targets) & USART1 CLI tab (pyserial).
"""

import ctypes
import ctypes.wintypes as wt
import glob
import os
import queue
import select
import shutil
import struct
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

# ---------------------------------------------------------------------------
# STM32 Device ID lookup (matches Core/Src/stm32_target.c)
# ---------------------------------------------------------------------------
STM32_DEVICES = {
    0x412: ("F1", 1024, "STM32F10x Low-density"),
    0x410: ("F1", 1024, "STM32F10x Medium-density (F103C8/RB)"),
    0x414: ("F1", 2048, "STM32F10x High-density (F103RC/ZE)"),
    0x418: ("F1", 2048, "STM32F105/F107 Connectivity"),
    0x420: ("F1", 1024, "STM32F100 Value Line"),
    0x428: ("F1", 2048, "STM32F100 High-density Value Line"),
    0x430: ("F1", 2048, "STM32F10x XL-density"),
    0x422: ("F3", 2048, "STM32F302/F303xB/C"),
    0x432: ("F3", 2048, "STM32F373"),
    0x438: ("F3", 2048, "STM32F303x6/8 / F334"),
    0x439: ("F3", 2048, "STM32F301/F302x6/8"),
    0x446: ("F3", 2048, "STM32F302/F303xD/E"),
    0x411: ("F2", 16384, "STM32F20x/F21x"),
    0x413: ("F4", 16384, "STM32F405/F407/F415/F417"),
    0x419: ("F4", 16384, "STM32F427/F429/F437/F439"),
    0x421: ("F4", 16384, "STM32F446"),
    0x423: ("F4", 16384, "STM32F401xB/C"),
    0x431: ("F4", 16384, "STM32F411xC/E"),
    0x433: ("F4", 16384, "STM32F401xD/E"),
    0x434: ("F4", 16384, "STM32F469/F479"),
    0x441: ("F4", 16384, "STM32F412"),
    0x458: ("F4", 16384, "STM32F410"),
    0x463: ("F4", 16384, "STM32F413/F423"),
    0x468: ("G4", 2048, "STM32G431/G441 (Cat.2)"),
    0x469: ("G4", 2048, "STM32G473/G474/G484 (Cat.3)"),
    0x479: ("G4", 2048, "STM32G491/G4A1 (Cat.4)"),
    0x415: ("L4", 2048, "STM32L475/L476/L486"),
    0x435: ("L4", 2048, "STM32L43x/L44x"),
    0x462: ("L4", 2048, "STM32L45x/L46x"),
    0x464: ("L4", 2048, "STM32L41x/L42x"),
    0x440: ("F0", 1024, "STM32F05x/F030x8"),
    0x444: ("F0", 1024, "STM32F03x/F04x"),
    0x445: ("F0", 2048, "STM32F07x"),
    0x448: ("F0", 2048, "STM32F09x/F030xC"),
    0x460: ("G0", 2048, "STM32G07x/G08x"),
    0x466: ("G0", 2048, "STM32G03x/G04x"),
    0x467: ("G0", 2048, "STM32G0Bx/G0Cx"),
}

OPENOCD_TARGETS = [
    ("STM32F1 (stm32f1x.cfg)", "target/stm32f1x.cfg"),
    ("STM32F3 (stm32f3x.cfg)", "target/stm32f3x.cfg"),
    ("STM32F4 (stm32f4x.cfg)", "target/stm32f4x.cfg"),
    ("STM32G4 (stm32g4x.cfg)", "target/stm32g4x.cfg"),
    ("STM32L4 (stm32l4x.cfg)", "target/stm32l4x.cfg"),
    ("STM32F0 (stm32f0x.cfg)", "target/stm32f0x.cfg"),
    ("STM32G0 (stm32g0x.cfg)", "target/stm32g0x.cfg"),
    ("STM32H7 (stm32h7x.cfg)", "target/stm32h7x.cfg"),
]


# ---------------------------------------------------------------------------
# Pure Python Firmware Image Loaders (.bin / .hex / .elf)
# ---------------------------------------------------------------------------
def load_firmware_image(path: str, default_bin_addr: int = 0x08000000):
    """Returns (base_addr, bytes_payload) for .bin, .hex, or .elf."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".hex":
        return _parse_intel_hex(path)
    if ext == ".elf":
        return _parse_elf32(path)
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 4:
        raise ValueError("File .bin qua nho (< 4 bytes)")
    return default_bin_addr, data


def _parse_intel_hex(path: str):
    ext_addr = 0
    chunks = {}
    with open(path, "r", encoding="ascii", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":") or len(line) < 11:
                continue
            raw = bytes.fromhex(line[1:])
            length, addr_lo, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
            payload = raw[4 : 4 + length]
            if rtype == 0x00:
                abs_addr = ext_addr + addr_lo
                for i, b in enumerate(payload):
                    chunks[abs_addr + i] = b
            elif rtype == 0x01:
                break
            elif rtype == 0x02 and length == 2:
                ext_addr = ((payload[0] << 8) | payload[1]) << 4
            elif rtype == 0x04 and length == 2:
                ext_addr = ((payload[0] << 8) | payload[1]) << 16
    if not chunks:
        raise ValueError("File .hex khong co du lieu Flash")
    min_addr = min(chunks.keys())
    max_addr = max(chunks.keys())
    buf = bytearray([0xFF] * (max_addr - min_addr + 1))
    for a, b in chunks.items():
        buf[a - min_addr] = b
    return min_addr, bytes(buf)


def _parse_elf32(path: str):
    with open(path, "rb") as f:
        elf = f.read()
    if len(elf) < 52 or elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise ValueError("Khong phai file ELF 32-bit little-endian hop le")
    e_phoff = struct.unpack_from("<I", elf, 28)[0]
    e_phentsize = struct.unpack_from("<H", elf, 42)[0]
    e_phnum = struct.unpack_from("<H", elf, 44)[0]

    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, _p_vaddr, p_paddr, p_filesz, _p_memsz, _p_flags, _p_align = struct.unpack_from("<8I", elf, off)
        if p_type == 1 and p_filesz > 0 and 0x08000000 <= p_paddr < 0x08400000:
            segs.append((p_paddr, elf[p_offset : p_offset + p_filesz]))

    if not segs:
        raise ValueError("Khong tim thay segment Flash (0x08000000+) trong file .elf")
    segs.sort(key=lambda s: s[0])
    base = segs[0][0]
    end = max(addr + len(data) for addr, data in segs)
    buf = bytearray([0xFF] * (end - base))
    for addr, data in segs:
        buf[addr - base : addr - base + len(data)] = data
    return base, bytes(buf)


# ---------------------------------------------------------------------------
# Pure ctypes Windows HID Driver (no pip packages needed)
# ---------------------------------------------------------------------------
if os.name == "nt":
    hid_dll = ctypes.windll.hid
    setupapi = ctypes.windll.setupapi
    kernel32 = ctypes.windll.kernel32

    INVALID_HANDLE_VALUE = wt.HANDLE(-1).value
    DIGCF_PRESENT = 0x02
    DIGCF_DEVICEINTERFACE = 0x10
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x01
    FILE_SHARE_WRITE = 0x02
    OPEN_EXISTING = 3
    FILE_FLAG_OVERLAPPED = 0x40000000
    WAIT_OBJECT_0 = 0x00000000
    ERROR_IO_PENDING = 997

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", wt.DWORD),
            ("Data2", wt.WORD),
            ("Data3", wt.WORD),
            ("Data4", ctypes.c_ubyte * 8),
        ]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wt.DWORD),
            ("InterfaceClassGuid", GUID),
            ("Flags", wt.DWORD),
            ("Reserved", ctypes.c_void_p),
        ]

    class HIDD_ATTRIBUTES(ctypes.Structure):
        _fields_ = [
            ("Size", wt.ULONG),
            ("VendorID", wt.USHORT),
            ("ProductID", wt.USHORT),
            ("VersionNumber", wt.USHORT),
        ]

    class HIDP_CAPS(ctypes.Structure):
        _fields_ = [
            ("Usage", wt.USHORT),
            ("UsagePage", wt.USHORT),
            ("InputReportByteLength", wt.USHORT),
            ("OutputReportByteLength", wt.USHORT),
            ("FeatureReportByteLength", wt.USHORT),
            ("Reserved", wt.USHORT * 17),
            ("NumberLinkCollectionNodes", wt.USHORT),
            ("NumberInputButtonCaps", wt.USHORT),
            ("NumberInputValueCaps", wt.USHORT),
            ("NumberInputDataIndices", wt.USHORT),
            ("NumberOutputButtonCaps", wt.USHORT),
            ("NumberOutputValueCaps", wt.USHORT),
            ("NumberOutputDataIndices", wt.USHORT),
            ("NumberFeatureButtonCaps", wt.USHORT),
            ("NumberFeatureValueCaps", wt.USHORT),
            ("NumberFeatureDataIndices", wt.USHORT),
        ]

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [
            ("Internal", ctypes.c_void_p),
            ("InternalHigh", ctypes.c_void_p),
            ("Offset", wt.DWORD),
            ("OffsetHigh", wt.DWORD),
            ("hEvent", wt.HANDLE),
        ]

    setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)
    ]
    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        wt.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p
    ]
    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]

    kernel32.CreateFileW.restype = wt.HANDLE
    kernel32.CreateFileW.argtypes = [
        wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE
    ]
    kernel32.CloseHandle.argtypes = [wt.HANDLE]
    kernel32.CreateEventW.restype = wt.HANDLE
    kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    kernel32.GetOverlappedResult.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.BOOL]
    kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED)]
    kernel32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
    kernel32.ReadFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]

    hid_dll.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
    hid_dll.HidD_GetAttributes.argtypes = [wt.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
    hid_dll.HidD_GetProductString.argtypes = [wt.HANDLE, wt.LPWSTR, wt.ULONG]
    hid_dll.HidD_GetManufacturerString.argtypes = [wt.HANDLE, wt.LPWSTR, wt.ULONG]
    hid_dll.HidD_GetPreparsedData.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    hid_dll.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid_dll.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]


def enumerate_hid_devices():
    if os.name == "nt":
        return _enumerate_hid_windows()
    return _enumerate_hid_linux()


def _enumerate_hid_linux():
    devices = []
    for sys_path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        name = os.path.basename(sys_path)
        dev_node = f"/dev/{name}"
        uevent_path = os.path.join(sys_path, "device", "uevent")
        vid, pid, prod = 0, 0, ""
        try:
            with open(uevent_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("HID_ID="):
                        parts = line.strip().split("=", 1)[1].split(":")
                        if len(parts) == 3:
                            vid = int(parts[1], 16)
                            pid = int(parts[2], 16)
                    elif line.startswith("HID_NAME="):
                        prod = line.strip().split("=", 1)[1]
        except OSError:
            continue

        usage_page = 0
        rdesc_path = os.path.join(sys_path, "device", "report_descriptor")
        try:
            with open(rdesc_path, "rb") as f:
                rdesc = f.read(4)
                if len(rdesc) >= 3 and rdesc[0] == 0x06:
                    usage_page = rdesc[1] | (rdesc[2] << 8)
        except OSError:
            pass

        devices.append({
            "path": dev_node,
            "vid": vid,
            "pid": pid,
            "mfg": "",
            "prod": prod,
            "usage_page": usage_page,
            "in_len": 64,
            "out_len": 65,
        })
    return devices


def _enumerate_hid_windows():
    devices = []
    guid = GUID()
    hid_dll.HidD_GetHidGuid(ctypes.byref(guid))
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if hdev == INVALID_HANDLE_VALUE:
        return devices
    try:
        did = SP_DEVICE_INTERFACE_DATA()
        did.cbSize = ctypes.sizeof(did)
        idx = 0
        while setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(guid), idx, ctypes.byref(did)):
            idx += 1
            req = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None, 0, ctypes.byref(req), None)
            if req.value == 0:
                continue
            buf = ctypes.create_string_buffer(req.value)
            cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = cb
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), buf, req.value, None, None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
            fh = kernel32.CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
            if fh == INVALID_HANDLE_VALUE:
                continue
            try:
                attr = HIDD_ATTRIBUTES()
                attr.Size = ctypes.sizeof(attr)
                if not hid_dll.HidD_GetAttributes(fh, ctypes.byref(attr)):
                    continue
                sbuf = ctypes.create_unicode_buffer(128)
                prod = sbuf.value if hid_dll.HidD_GetProductString(fh, sbuf, 256) else ""
                sbuf2 = ctypes.create_unicode_buffer(128)
                mfg = sbuf2.value if hid_dll.HidD_GetManufacturerString(fh, sbuf2, 256) else ""
                ppd = ctypes.c_void_p()
                in_len, out_len, usage_page = 65, 65, 0
                if hid_dll.HidD_GetPreparsedData(fh, ctypes.byref(ppd)):
                    caps = HIDP_CAPS()
                    if hid_dll.HidP_GetCaps(ppd, ctypes.byref(caps)) == 0x00110000:
                        in_len = caps.InputReportByteLength
                        out_len = caps.OutputReportByteLength
                        usage_page = caps.UsagePage
                    hid_dll.HidD_FreePreparsedData(ppd)
                devices.append({
                    "path": path,
                    "vid": attr.VendorID,
                    "pid": attr.ProductID,
                    "mfg": mfg,
                    "prod": prod,
                    "usage_page": usage_page,
                    "in_len": in_len,
                    "out_len": out_len,
                })
            finally:
                kernel32.CloseHandle(fh)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(hdev)
    return devices


class WinHidDevice:
    def __init__(self, path, in_len=65, out_len=65):
        self.path = path
        self.in_len = in_len
        self.out_len = out_len
        self.handle = kernel32.CreateFileW(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            None,
        )
        if self.handle == INVALID_HANDLE_VALUE:
            err = ctypes.get_last_error()
            raise OSError(f"Cannot open HID device (WinErr={err})")

    def close(self):
        if self.handle and self.handle != INVALID_HANDLE_VALUE:
            kernel32.CloseHandle(self.handle)
            self.handle = INVALID_HANDLE_VALUE

    def exchange(self, payload: bytes, timeout_ms: int = 2000) -> bytes:
        out_buf = bytearray(self.out_len)
        out_buf[0] = 0x00
        n = min(len(payload), self.out_len - 1)
        out_buf[1 : 1 + n] = payload[:n]
        raw_out = (ctypes.c_ubyte * self.out_len).from_buffer(out_buf)

        ev_w = kernel32.CreateEventW(None, True, False, None)
        ov_w = OVERLAPPED()
        ov_w.hEvent = ev_w
        written = wt.DWORD(0)
        ok_w = kernel32.WriteFile(self.handle, raw_out, self.out_len, ctypes.byref(written), ctypes.byref(ov_w))
        if not ok_w:
            err = ctypes.GetLastError()
            if err != ERROR_IO_PENDING:
                kernel32.CloseHandle(ev_w)
                raise OSError(f"HID WriteFile failed (err={err})")
            if kernel32.WaitForSingleObject(ev_w, timeout_ms) != WAIT_OBJECT_0:
                kernel32.CancelIoEx(self.handle, ctypes.byref(ov_w))
                kernel32.CloseHandle(ev_w)
                raise TimeoutError("HID Write timeout")
            kernel32.GetOverlappedResult(self.handle, ctypes.byref(ov_w), ctypes.byref(written), False)
        kernel32.CloseHandle(ev_w)

        in_buf = (ctypes.c_ubyte * self.in_len)()
        ev_r = kernel32.CreateEventW(None, True, False, None)
        ov_r = OVERLAPPED()
        ov_r.hEvent = ev_r
        read_n = wt.DWORD(0)
        ok_r = kernel32.ReadFile(self.handle, in_buf, self.in_len, ctypes.byref(read_n), ctypes.byref(ov_r))
        if not ok_r:
            err = ctypes.GetLastError()
            if err != ERROR_IO_PENDING:
                kernel32.CloseHandle(ev_r)
                raise OSError(f"HID ReadFile failed (err={err})")
            if kernel32.WaitForSingleObject(ev_r, timeout_ms) != WAIT_OBJECT_0:
                kernel32.CancelIoEx(self.handle, ctypes.byref(ov_r))
                kernel32.CloseHandle(ev_r)
                raise TimeoutError("HID Read timeout")
            kernel32.GetOverlappedResult(self.handle, ctypes.byref(ov_r), ctypes.byref(read_n), False)
        kernel32.CloseHandle(ev_r)

        data = bytes(in_buf[: read_n.value])
        return data[1:] if len(data) > 1 and data[0] == 0x00 else data


class LinuxHidDevice:
    def __init__(self, path, in_len=64, out_len=65):
        self.path = path
        self.in_len = in_len
        self.out_len = out_len
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except PermissionError as e:
            raise PermissionError(
                f"Khong co quyen mo {path}. Hay cai udev rule (99-tue-cmsis-dap.rules) hoac chay voi sudo!"
            ) from e

    def close(self):
        if self.fd is not None and self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def exchange(self, payload: bytes, timeout_ms: int = 2000) -> bytes:
        # Linux hidraw_write expects byte 0 = Report ID (0x00 for unnumbered reports)
        out_buf = bytearray(65)
        out_buf[0] = 0x00
        n = min(len(payload), 64)
        out_buf[1 : 1 + n] = payload[:n]
        os.write(self.fd, bytes(out_buf))

        rlist, _, _ = select.select([self.fd], [], [], timeout_ms / 1000.0)
        if not rlist:
            raise TimeoutError("HID Read timeout (hidraw)")
        data = os.read(self.fd, 65)
        return data[1:] if len(data) == 65 and data[0] == 0x00 else data


def open_hid_device(dev_info: dict):
    if os.name == "nt":
        return WinHidDevice(dev_info["path"], dev_info["in_len"], dev_info["out_len"])
    return LinuxHidDevice(dev_info["path"], dev_info["in_len"], dev_info["out_len"])


# ---------------------------------------------------------------------------
# CMSIS-DAP Client + Multi-STM32 Flash Programmer (No OpenOCD Required!)
# ---------------------------------------------------------------------------
class CmsisDapClient:
    ID_DAP_INFO = 0x00
    ID_DAP_HOST_STATUS = 0x01
    ID_DAP_CONNECT = 0x02
    ID_DAP_DISCONNECT = 0x03
    ID_DAP_TRANSFER_CONFIGURE = 0x04
    ID_DAP_TRANSFER = 0x05
    ID_DAP_TRANSFER_BLOCK = 0x06
    ID_DAP_WRITE_ABORT = 0x08
    ID_DAP_RESET_TARGET = 0x0A
    ID_DAP_SWJ_PINS = 0x10
    ID_DAP_SWJ_CLOCK = 0x11
    ID_DAP_SWJ_SEQUENCE = 0x12
    ID_DAP_SWD_CONFIGURE = 0x13

    FLASH_KEY1 = 0x45670123
    FLASH_KEY2 = 0xCDEF89AB

    def __init__(self, hid_dev: WinHidDevice):
        self.dev = hid_dev

    def cmd(self, payload: bytes) -> bytes:
        resp = self.dev.exchange(payload)
        if not resp or resp[0] != payload[0]:
            raise RuntimeError(f"DAP command 0x{payload[0]:02X} invalid response: {resp[:8].hex()}")
        return resp

    def info_str(self, info_id: int) -> str:
        r = self.cmd(bytes([self.ID_DAP_INFO, info_id]))
        n = r[1]
        return "" if n == 0 else r[2 : 2 + n].rstrip(b"\x00").decode("utf-8", errors="replace")

    def info_bytes(self, info_id: int) -> bytes:
        r = self.cmd(bytes([self.ID_DAP_INFO, info_id]))
        return r[2 : 2 + r[1]]

    def host_status(self, led_type: int, on: bool):
        self.cmd(bytes([self.ID_DAP_HOST_STATUS, led_type, 1 if on else 0]))

    def connect_swd(self) -> int:
        return self.cmd(bytes([self.ID_DAP_CONNECT, 0x01]))[1]

    def disconnect(self):
        self.cmd(bytes([self.ID_DAP_DISCONNECT]))

    def swj_clock(self, hz: int):
        self.cmd(bytes([self.ID_DAP_SWJ_CLOCK]) + struct.pack("<I", hz))

    def transfer_configure(self, idle=2, wait_retry=500, match_retry=0):
        self.cmd(bytes([self.ID_DAP_TRANSFER_CONFIGURE, idle]) + struct.pack("<HH", wait_retry, match_retry))

    def swd_configure(self, cfg=0):
        self.cmd(bytes([self.ID_DAP_SWD_CONFIGURE, cfg]))

    def swj_sequence(self, bit_count: int, data: bytes):
        cnt = 0 if bit_count >= 256 else bit_count
        self.cmd(bytes([self.ID_DAP_SWJ_SEQUENCE, cnt]) + data)

    def swj_pins(self, pin_out: int, pin_sel: int, wait_us: int = 0) -> int:
        return self.cmd(bytes([self.ID_DAP_SWJ_PINS, pin_out, pin_sel]) + struct.pack("<I", wait_us))[1]

    def reset_target(self) -> int:
        return self.cmd(bytes([self.ID_DAP_RESET_TARGET]))[1]

    def write_abort(self, val: int = 0x1E):
        self.cmd(bytes([self.ID_DAP_WRITE_ABORT, 0]) + struct.pack("<I", val))

    def transfer(self, ops):
        buf = bytearray([self.ID_DAP_TRANSFER, 0x00, len(ops)])
        read_count = 0
        for req_b, val in ops:
            buf.append(req_b)
            if (req_b & 0x02) == 0:
                buf += struct.pack("<I", val)
            else:
                read_count += 1
        r = self.cmd(bytes(buf))
        cnt, ack = r[1], r[2]
        vals = []
        off = 3
        while off + 4 <= len(r) and len(vals) < read_count:
            vals.append(struct.unpack_from("<I", r, off)[0])
            off += 4
        return cnt, ack, vals

    def transfer_block_write(self, req_byte: int, words):
        # Max 14 words in a 64-byte HID packet (5 + 14*4 = 61 bytes)
        for i in range(0, len(words), 14):
            chunk = words[i : i + 14]
            buf = bytearray([self.ID_DAP_TRANSFER_BLOCK, 0x00])
            buf += struct.pack("<HB", len(chunk), req_byte & ~0x02)
            for w in chunk:
                buf += struct.pack("<I", w)
            r = self.cmd(bytes(buf))
            ack = r[3]
            if ack != 1:
                raise RuntimeError(f"DAP_TransferBlock write failed (ACK={ack})")

    def transfer_block_read(self, req_byte: int, count: int):
        # Max 15 words in a 64-byte HID response (4 + 15*4 = 64 bytes)
        out = []
        rem = count
        while rem > 0:
            n = min(rem, 15)
            buf = bytearray([self.ID_DAP_TRANSFER_BLOCK, 0x00])
            buf += struct.pack("<HB", n, req_byte | 0x02)
            r = self.cmd(bytes(buf))
            got_cnt = struct.unpack_from("<H", r, 1)[0]
            ack = r[3]
            if ack != 1 or got_cnt != n:
                raise RuntimeError(f"DAP_TransferBlock read failed (ACK={ack}, got={got_cnt}/{n})")
            for j in range(n):
                out.append(struct.unpack_from("<I", r, 4 + j * 4)[0])
            rem -= n
        return out

    def swd_line_reset_to_swd(self):
        self.swj_sequence(56, b"\xFF" * 7)
        self.swj_sequence(16, b"\x9E\xE7")
        self.swj_sequence(56, b"\xFF" * 7)
        self.swj_sequence(8, b"\x00")

    def read_dp(self, addr: int) -> int:
        _, ack, vals = self.transfer([(0x02 | (addr & 0x0C), 0)])
        if ack != 1:
            raise RuntimeError(f"SWD DP read 0x{addr:02X} failed (ACK={ack})")
        return vals[0]

    def write_dp(self, addr: int, val: int):
        _, ack, _ = self.transfer([(0x00 | (addr & 0x0C), val)])
        if ack != 1:
            raise RuntimeError(f"SWD DP write 0x{addr:02X} failed (ACK={ack})")

    def mem_read32(self, addr: int) -> int:
        _, ack, vals = self.transfer([(0x01 | 0x04, addr), (0x01 | 0x02 | 0x0C, 0)])
        if ack != 1:
            raise RuntimeError(f"SWD memRead32 0x{addr:08X} failed (ACK={ack})")
        return vals[0]

    def mem_write32(self, addr: int, val: int):
        _, ack, _ = self.transfer([(0x01 | 0x04, addr), (0x01 | 0x0C, val)])
        if ack != 1:
            raise RuntimeError(f"SWD memWrite32 0x{addr:08X} failed (ACK={ack})")

    def mem_write16_batch(self, start_addr: int, halfwords):
        """Fast 16-bit half-word batching via DAP_Transfer (6 halfwords per HID packet)."""
        _, ack, _ = self.transfer([(0x01 | 0x00, 0x23000011)])  # AP_CSW = 16-bit auto-inc
        if ack != 1:
            raise RuntimeError(f"AP_CSW 16-bit write failed (ACK={ack})")
        try:
            addr = start_addr
            for i in range(0, len(halfwords), 6):
                chunk = halfwords[i : i + 6]
                ops = []
                for hw in chunk:
                    packed = (hw << 16) if (addr & 2) else hw
                    ops.append((0x01 | 0x04, addr))
                    ops.append((0x01 | 0x0C, packed))
                    addr += 2
                _, ack, _ = self.transfer(ops)
                if ack != 1:
                    raise RuntimeError(f"SWD write16 batch failed near 0x{addr:08X} (ACK={ack})")
        finally:
            self.transfer([(0x01 | 0x00, 0x23000012)])  # Restore 32-bit auto-inc

    def mem_write_block(self, addr: int, words):
        idx = 0
        total = len(words)
        while idx < total:
            room = (0x400 - (addr & 0x3FF)) // 4
            n = min(total - idx, room)
            _, ack, _ = self.transfer([(0x01 | 0x04, addr)])
            if ack != 1:
                raise RuntimeError(f"AP_TAR write failed at 0x{addr:08X} (ACK={ack})")
            self.transfer_block_write(0x01 | 0x0C, words[idx : idx + n])
            addr += n * 4
            idx += n

    def mem_read_block(self, addr: int, count: int):
        out = []
        while count > 0:
            room = (0x400 - (addr & 0x3FF)) // 4
            n = min(count, room)
            _, ack, _ = self.transfer([(0x01 | 0x04, addr)])
            if ack != 1:
                raise RuntimeError(f"AP_TAR write failed at 0x{addr:08X} (ACK={ack})")
            out.extend(self.transfer_block_read(0x01 | 0x02 | 0x0C, n))
            addr += n * 4
            count -= n
        return out

    def _attach_and_halt(self, clock_hz: int, connect_under_reset: bool):
        if self.connect_swd() != 1:
            raise RuntimeError("DAP_Connect(SWD) failed")
        self.swj_clock(clock_hz)
        self.transfer_configure(2, 500, 0)
        self.swd_configure(0)

        if connect_under_reset:
            self.swj_pins(0x00, 0x80, 0)  # Assert nRESET LOW
            time.sleep(0.015)

        self.swd_line_reset_to_swd()
        dp_id = self.read_dp(0x00)
        self.write_abort(0x1E)
        self.write_dp(0x08, 0x00000000)
        self.write_dp(0x04, 0x50000000)
        for _ in range(50):
            if (self.read_dp(0x04) & 0xA0000000) == 0xA0000000:
                break
            time.sleep(0.002)
        else:
            raise RuntimeError("Debug power-up timeout")

        _, ack, _ = self.transfer([(0x01 | 0x00, 0x23000012)])
        if ack != 1:
            raise RuntimeError(f"AP_CSW write failed (ACK={ack})")

        if connect_under_reset:
            self.mem_write32(0xE000EDF0, 0xA05F0003)  # DHCSR_HALT
            self.mem_write32(0xE000EDFC, 0x00000001)  # DEMCR_VC_CORERESET
            self.swj_pins(0x80, 0x80, 0)              # Release nRESET HIGH
            time.sleep(0.02)
        else:
            self.mem_write32(0xE000EDF0, 0xA05F0003)

        for _ in range(100):
            if self.mem_read32(0xE000EDF0) & (1 << 17):
                break
            time.sleep(0.002)
        else:
            raise RuntimeError("CPU halt timeout")
        if connect_under_reset:
            self.mem_write32(0xE000EDFC, 0x00000000)
        return dp_id

    def _identify_halted_target(self, dp_id: int) -> dict:
        dbg_id = self.mem_read32(0xE0042000)
        if (dbg_id & 0xFFF) == 0:
            dbg_id = self.mem_read32(0x40015800)

        dev_id = dbg_id & 0xFFF
        rev_id = (dbg_id >> 16) & 0xFFFF
        fam, page_sz, name = STM32_DEVICES.get(dev_id, ("?", 2048, "Unknown STM32 / Cortex-M"))

        flash_kb = 0
        rdp = None
        if fam in ("F1", "F3"):
            flash_kb = self.mem_read32(0x1FFFF7E0) & 0xFFFF
            obr = self.mem_read32(0x4002201C)
            rdp = 0xBB if (obr & 2) else 0xAA
            uid_base = 0x1FFFF7E8
        elif fam in ("F2", "F4"):
            flash_kb = (self.mem_read32(0x1FFF7A20) >> 16) & 0xFFFF
            optcr = self.mem_read32(0x40023C14)
            rdp = (optcr >> 8) & 0xFF
            uid_base = 0x1FFF7A10
        elif fam in ("G4", "L4"):
            flash_kb = self.mem_read32(0x1FFF75E0) & 0xFFFF
            optr = self.mem_read32(0x40022020)
            rdp = optr & 0xFF
            uid_base = 0x1FFF7590
        elif fam == "F0":
            flash_kb = self.mem_read32(0x1FFFF7CC) & 0xFFFF
            uid_base = 0x1FFFF7AC
        elif fam == "G0":
            flash_kb = self.mem_read32(0x1FFF75E0) & 0xFFFF
            uid_base = 0x1FFF7590
        else:
            uid_base = None

        uid_hex = "N/A"
        if uid_base is not None:
            u0 = self.mem_read32(uid_base)
            u1 = self.mem_read32(uid_base + 4)
            u2 = self.mem_read32(uid_base + 8)
            uid_hex = f"{u2:08X}-{u1:08X}-{u0:08X}"

        if flash_kb == 0xFFFF:
            flash_kb = 0
        if fam != "F0" and fam != "G0":
            fz = self.mem_read32(0xE0042008)
            self.mem_write32(0xE0042008, fz | (1 << 11) | (1 << 12))  # Freeze IWDG/WWDG

        return {
            "dp_id": dp_id,
            "dbg_id": dbg_id,
            "dev_id": dev_id,
            "rev_id": rev_id,
            "family": fam,
            "page_sz": page_sz,
            "name": name,
            "flash_kb": flash_kb,
            "rdp": rdp,
            "uid": uid_hex,
        }

    def probe_target(self, clock_hz: int = 500_000, connect_under_reset: bool = False) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            self.mem_write32(0xE000EDF0, 0xA05F0001)  # Resume
            return info
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def _wait_flash_ready(self, sr_addr: int, bsy_mask: int, err_mask: int, eop_mask: int, timeout_s: float):
        t0 = time.time()
        while True:
            sr = self.mem_read32(sr_addr)
            if sr & err_mask:
                self.mem_write32(sr_addr, err_mask | eop_mask)
                raise RuntimeError(f"Flash controller error SR=0x{sr:08X}")
            if (sr & bsy_mask) == 0:
                return
            if (time.time() - t0) > timeout_s:
                raise TimeoutError(f"Flash timeout SR=0x{sr:08X}")
            time.sleep(0.002)

    def flash_image(self, base_addr: int, data: bytes, clock_hz: int = 500_000,
                    connect_under_reset: bool = False, verify: bool = True, progress_cb=None) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            fam = info["family"]
            if info["rdp"] is not None and info["rdp"] != 0xAA:
                raise RuntimeError(f"Chip dang bat Read-Out Protection (RDP=0x{info['rdp']:02X})")
            if fam not in ("F1", "F3", "F2", "F4", "G4", "L4"):
                raise RuntimeError(f"Chua ho tro nap truc tiep dong {info['name']} (DEV_ID=0x{info['dev_id']:03X})")

            if info["flash_kb"] > 0:
                flash_end = 0x08000000 + info["flash_kb"] * 1024
                if base_addr + len(data) > flash_end:
                    raise RuntimeError(f"Firmware ({len(data)} B) vuot qua Flash ({info['flash_kb']} KB)")

            # Pad to 8-byte boundary with 0xFF
            padded = bytearray(data)
            while len(padded) % 8 != 0:
                padded.append(0xFF)
            total_len = len(padded)

            if fam in ("F1", "F3"):
                self._flash_f1_f3(base_addr, padded, info["page_sz"], progress_cb)
            elif fam in ("F2", "F4"):
                self._flash_f2_f4(base_addr, padded, progress_cb)
            elif fam in ("G4", "L4"):
                self._flash_g4_l4(base_addr, padded, info["page_sz"], progress_cb)

            if verify:
                words_want = list(struct.unpack(f"<{total_len // 4}I", padded))
                step = 256  # 1 KB
                for off_w in range(0, len(words_want), step):
                    cnt = min(step, len(words_want) - off_w)
                    got = self.mem_read_block(base_addr + off_w * 4, cnt)
                    for i in range(cnt):
                        if got[i] != words_want[off_w + i]:
                            bad_addr = base_addr + (off_w + i) * 4
                            raise RuntimeError(f"Verify mismatch @ 0x{bad_addr:08X}: wrote 0x{words_want[off_w + i]:08X} read 0x{got[i]:08X}")
                    if progress_cb:
                        progress_cb("verify", min((off_w + cnt) * 4, total_len), total_len)

            # Reset & run target
            self.mem_write32(0xE000EDFC, 0x00000000)
            self.mem_write32(0xE000EDF0, 0xA05F0001)
            try:
                self.mem_write32(0xE000ED0C, 0x05FA0004)  # AIRCR SYSRESETREQ
            except Exception:
                pass
            time.sleep(0.03)
            self.reset_target()
            return info
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def _flash_f1_f3(self, base_addr: int, padded: bytearray, page_sz: int, progress_cb):
        CR, SR, KEYR, AR = 0x40022010, 0x4002200C, 0x40022004, 0x40022014
        if self.mem_read32(CR) & (1 << 7):
            self.mem_write32(KEYR, self.FLASH_KEY1)
            self.mem_write32(KEYR, self.FLASH_KEY2)
            if self.mem_read32(CR) & (1 << 7):
                raise RuntimeError("F1/F3 Flash unlock failed")
        try:
            start_page = (base_addr - 0x08000000) // page_sz
            end_page = (base_addr + len(padded) - 1 - 0x08000000) // page_sz
            num_pages = end_page - start_page + 1
            for idx, p in enumerate(range(start_page, end_page + 1)):
                if progress_cb:
                    progress_cb("erase", idx, num_pages)
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 0.5)
                self.mem_write32(CR, 0x02)  # PER
                self.mem_write32(AR, 0x08000000 + p * page_sz)
                self.mem_write32(CR, 0x42)  # PER | STRT
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 1.0)
            if progress_cb:
                progress_cb("erase", num_pages, num_pages)

            self.mem_write32(CR, 0x01)  # PG
            hw_all = list(struct.unpack(f"<{len(padded) // 2}H", padded))
            batch_hw = 128  # 256 bytes per step
            for off_h in range(0, len(hw_all), batch_hw):
                chunk = hw_all[off_h : off_h + batch_hw]
                self.mem_write16_batch(base_addr + off_h * 2, chunk)
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 0.5)
                if progress_cb:
                    progress_cb("program", min((off_h + len(chunk)) * 2, len(padded)), len(padded))
        finally:
            self.mem_write32(CR, 1 << 7)

    def _flash_f2_f4(self, base_addr: int, padded: bytearray, progress_cb):
        CR, SR, KEYR = 0x40023C10, 0x40023C0C, 0x40023C04
        if self.mem_read32(CR) & (1 << 31):
            self.mem_write32(KEYR, self.FLASH_KEY1)
            self.mem_write32(KEYR, self.FLASH_KEY2)
            if self.mem_read32(CR) & (1 << 31):
                raise RuntimeError("F4 Flash unlock failed")
        try:
            sector_sizes = [16384, 16384, 16384, 16384, 65536] + [131072] * 7
            sectors_to_erase = []
            cur = 0x08000000
            img_end = base_addr + len(padded)
            for idx, sz in enumerate(sector_sizes):
                if cur + sz > base_addr and cur < img_end:
                    sectors_to_erase.append(idx)
                cur += sz
            for i, s in enumerate(sectors_to_erase):
                if progress_cb:
                    progress_cb("erase", i, len(sectors_to_erase))
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 0.5)
                self.mem_write32(SR, 0x1F3)
                self.mem_write32(CR, (1 << 1) | (s << 3) | (2 << 8))
                self.mem_write32(CR, (1 << 1) | (s << 3) | (2 << 8) | (1 << 16))
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 5.0)
            if progress_cb:
                progress_cb("erase", len(sectors_to_erase), len(sectors_to_erase))

            self.mem_write32(CR, (1 << 0) | (2 << 8))  # PG | PSIZE_X32
            words = list(struct.unpack(f"<{len(padded) // 4}I", padded))
            step = 128  # 512 bytes per block
            for off_w in range(0, len(words), step):
                chunk = words[off_w : off_w + step]
                self.mem_write_block(base_addr + off_w * 4, chunk)
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 0.5)
                if progress_cb:
                    progress_cb("program", min((off_w + len(chunk)) * 4, len(padded)), len(padded))
        finally:
            self.mem_write32(CR, 1 << 31)

    def read_memory_bytes(self, addr: int, length: int, clock_hz: int = 500_000,
                          connect_under_reset: bool = False, progress_cb=None) -> bytes:
        try:
            self._attach_and_halt(clock_hz, connect_under_reset)
            aligned_start = addr & ~0x3
            aligned_end = (addr + length + 3) & ~0x3
            total_words = (aligned_end - aligned_start) // 4
            words = []
            step = 256
            for off_w in range(0, total_words, step):
                cnt = min(step, total_words - off_w)
                words.extend(self.mem_read_block(aligned_start + off_w * 4, cnt))
                if progress_cb:
                    progress_cb("read", min((off_w + cnt) * 4, total_words * 4), total_words * 4)
            raw = struct.pack(f"<{len(words)}I", *words)
            skip = addr - aligned_start
            self.mem_write32(0xE000EDF0, 0xA05F0001)  # Resume
            return raw[skip : skip + length]
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def verify_image(self, base_addr: int, data: bytes, clock_hz: int = 500_000,
                     connect_under_reset: bool = False, progress_cb=None) -> tuple:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            padded = bytearray(data)
            while len(padded) % 4 != 0:
                padded.append(0xFF)
            words_want = list(struct.unpack(f"<{len(padded) // 4}I", padded))
            step = 256
            mismatches = 0
            first_bad = None
            for off_w in range(0, len(words_want), step):
                cnt = min(step, len(words_want) - off_w)
                got = self.mem_read_block(base_addr + off_w * 4, cnt)
                for i in range(cnt):
                    if got[i] != words_want[off_w + i]:
                        mismatches += 1
                        if first_bad is None:
                            first_bad = (base_addr + (off_w + i) * 4, words_want[off_w + i], got[i])
                if progress_cb:
                    progress_cb("verify", min((off_w + cnt) * 4, len(padded)), len(padded))
            self.mem_write32(0xE000EDF0, 0xA05F0001)
            return info, mismatches, first_bad
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def mass_erase(self, clock_hz: int = 500_000, connect_under_reset: bool = False) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            fam = info["family"]
            if info["rdp"] is not None and info["rdp"] != 0xAA:
                raise RuntimeError(f"Chip dang bat RDP=0x{info['rdp']:02X}")

            if fam in ("F1", "F3"):
                CR, SR, KEYR = 0x40022010, 0x4002200C, 0x40022004
                if self.mem_read32(CR) & (1 << 7):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 1.0)
                self.mem_write32(CR, 1 << 2)            # MER
                self.mem_write32(CR, (1 << 2) | (1 << 6))  # MER | STRT
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 5.0)
                self.mem_write32(CR, 1 << 7)
            elif fam in ("F2", "F4"):
                CR, SR, KEYR = 0x40023C10, 0x40023C0C, 0x40023C04
                if self.mem_read32(CR) & (1 << 31):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 1.0)
                self.mem_write32(SR, 0x1F3)
                self.mem_write32(CR, (1 << 2) | (2 << 8))             # MER | PSIZE_X32
                self.mem_write32(CR, (1 << 2) | (2 << 8) | (1 << 16)) # | STRT
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 15.0)
                self.mem_write32(CR, 1 << 31)
            elif fam in ("G4", "L4"):
                CR, SR, KEYR = 0x40022014, 0x40022010, 0x40022008
                if self.mem_read32(CR) & (1 << 31):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 1.0)
                self.mem_write32(SR, 0x3FB)
                self.mem_write32(CR, 1 << 2)             # MER1
                self.mem_write32(CR, (1 << 2) | (1 << 16))
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 10.0)
                self.mem_write32(CR, 1 << 31)
            else:
                raise RuntimeError(f"Chua ho tro Mass Erase cho {info['name']}")
            return info
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def _read_core_reg(self, reg_idx: int) -> int:
        self.mem_write32(0xE000EDF4, reg_idx & 0x1F)  # DCRSR
        for _ in range(30):
            if self.mem_read32(0xE000EDF0) & (1 << 16):  # S_REGRDY
                return self.mem_read32(0xE000EDF8)       # DCRDR
        raise TimeoutError(f"DCRSR reg {reg_idx} timeout")

    def read_cpu_status(self, clock_hz: int = 500_000, connect_under_reset: bool = False) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            regs = {
                "R0": self._read_core_reg(0),
                "R1": self._read_core_reg(1),
                "R2": self._read_core_reg(2),
                "R3": self._read_core_reg(3),
                "R12": self._read_core_reg(12),
                "SP": self._read_core_reg(13),
                "LR": self._read_core_reg(14),
                "PC": self._read_core_reg(15),
                "xPSR": self._read_core_reg(16),
                "MSP": self._read_core_reg(17),
                "PSP": self._read_core_reg(18),
            }
            cfsr = self.mem_read32(0xE000ED28)
            hfsr = self.mem_read32(0xE000ED2C)
            return {"info": info, "regs": regs, "cfsr": cfsr, "hfsr": hfsr}
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def cpu_control(self, action: str, clock_hz: int = 500_000) -> str:
        if self.connect_swd() != 1:
            raise RuntimeError("DAP_Connect(SWD) failed")
        try:
            self.swj_clock(clock_hz)
            self.transfer_configure(2, 500, 0)
            self.swd_configure(0)
            self.swd_line_reset_to_swd()
            self.read_dp(0x00)
            self.write_abort(0x1E)
            self.write_dp(0x08, 0)
            self.write_dp(0x04, 0x50000000)
            for _ in range(30):
                if (self.read_dp(0x04) & 0xA0000000) == 0xA0000000:
                    break
                time.sleep(0.002)
            self.transfer([(0x01 | 0x00, 0x23000012)])

            if action == "halt":
                self.mem_write32(0xE000EDF0, 0xA05F0003)
                return "CPU HALTED"
            elif action == "run":
                self.mem_write32(0xE000EDF0, 0xA05F0001)
                return "CPU RUNNING"
            elif action == "step":
                self.mem_write32(0xE000EDF0, 0xA05F0003)
                self.mem_write32(0xE000EDF0, 0xA05F0005)  # C_STEP | C_DEBUGEN
                pc = self._read_core_reg(15)
                return f"CPU STEP -> PC=0x{pc:08X}"
            else:
                raise ValueError(action)
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def _flash_g4_l4(self, base_addr: int, padded: bytearray, page_sz: int, progress_cb):
        CR, SR, KEYR = 0x40022014, 0x40022010, 0x40022008
        if self.mem_read32(CR) & (1 << 31):
            self.mem_write32(KEYR, self.FLASH_KEY1)
            self.mem_write32(KEYR, self.FLASH_KEY2)
            if self.mem_read32(CR) & (1 << 31):
                raise RuntimeError("G4/L4 Flash unlock failed")
        try:
            start_page = (base_addr - 0x08000000) // page_sz
            end_page = (base_addr + len(padded) - 1 - 0x08000000) // page_sz
            num_pages = end_page - start_page + 1
            for idx, p in enumerate(range(start_page, end_page + 1)):
                if progress_cb:
                    progress_cb("erase", idx, num_pages)
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 0.5)
                self.mem_write32(SR, 0x3FB)
                self.mem_write32(CR, (1 << 1) | ((p & 0x7F) << 3))
                self.mem_write32(CR, (1 << 1) | ((p & 0x7F) << 3) | (1 << 16))
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 1.0)
            if progress_cb:
                progress_cb("erase", num_pages, num_pages)

            self.mem_write32(CR, 1 << 0)  # PG (64-bit double-word)
            words = list(struct.unpack(f"<{len(padded) // 4}I", padded))
            step = 128  # 512 bytes (64 double-words)
            for off_w in range(0, len(words), step):
                chunk = words[off_w : off_w + step]
                self.mem_write_block(base_addr + off_w * 4, chunk)
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 0.5)
                if progress_cb:
                    progress_cb("program", min((off_w + len(chunk)) * 4, len(padded)), len(padded))
        finally:
            self.mem_write32(CR, 1 << 31)

    def read_memory_bytes(self, addr: int, length: int, clock_hz: int = 500_000,
                          connect_under_reset: bool = False, progress_cb=None) -> bytes:
        try:
            self._attach_and_halt(clock_hz, connect_under_reset)
            aligned_start = addr & ~0x3
            aligned_end = (addr + length + 3) & ~0x3
            total_words = (aligned_end - aligned_start) // 4
            words = []
            step = 256
            for off_w in range(0, total_words, step):
                cnt = min(step, total_words - off_w)
                words.extend(self.mem_read_block(aligned_start + off_w * 4, cnt))
                if progress_cb:
                    progress_cb("read", min((off_w + cnt) * 4, total_words * 4), total_words * 4)
            raw = struct.pack(f"<{len(words)}I", *words)
            skip = addr - aligned_start
            self.mem_write32(0xE000EDF0, 0xA05F0001)  # Resume
            return raw[skip : skip + length]
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def verify_image(self, base_addr: int, data: bytes, clock_hz: int = 500_000,
                     connect_under_reset: bool = False, progress_cb=None) -> tuple:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            padded = bytearray(data)
            while len(padded) % 4 != 0:
                padded.append(0xFF)
            words_want = list(struct.unpack(f"<{len(padded) // 4}I", padded))
            step = 256
            mismatches = 0
            first_bad = None
            for off_w in range(0, len(words_want), step):
                cnt = min(step, len(words_want) - off_w)
                got = self.mem_read_block(base_addr + off_w * 4, cnt)
                for i in range(cnt):
                    if got[i] != words_want[off_w + i]:
                        mismatches += 1
                        if first_bad is None:
                            first_bad = (base_addr + (off_w + i) * 4, words_want[off_w + i], got[i])
                if progress_cb:
                    progress_cb("verify", min((off_w + cnt) * 4, len(padded)), len(padded))
            self.mem_write32(0xE000EDF0, 0xA05F0001)
            return info, mismatches, first_bad
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def mass_erase(self, clock_hz: int = 500_000, connect_under_reset: bool = False) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            fam = info["family"]
            if info["rdp"] is not None and info["rdp"] != 0xAA:
                raise RuntimeError(f"Chip dang bat RDP=0x{info['rdp']:02X}")

            if fam in ("F1", "F3"):
                CR, SR, KEYR = 0x40022010, 0x4002200C, 0x40022004
                if self.mem_read32(CR) & (1 << 7):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 1.0)
                self.mem_write32(CR, 1 << 2)            # MER
                self.mem_write32(CR, (1 << 2) | (1 << 6))  # MER | STRT
                self._wait_flash_ready(SR, 1, 0x14, 0x20, 5.0)
                self.mem_write32(CR, 1 << 7)
            elif fam in ("F2", "F4"):
                CR, SR, KEYR = 0x40023C10, 0x40023C0C, 0x40023C04
                if self.mem_read32(CR) & (1 << 31):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 1.0)
                self.mem_write32(SR, 0x1F3)
                self.mem_write32(CR, (1 << 2) | (2 << 8))             # MER | PSIZE_X32
                self.mem_write32(CR, (1 << 2) | (2 << 8) | (1 << 16)) # | STRT
                self._wait_flash_ready(SR, 1 << 16, 0x1F2, 1, 15.0)
                self.mem_write32(CR, 1 << 31)
            elif fam in ("G4", "L4"):
                CR, SR, KEYR = 0x40022014, 0x40022010, 0x40022008
                if self.mem_read32(CR) & (1 << 31):
                    self.mem_write32(KEYR, self.FLASH_KEY1)
                    self.mem_write32(KEYR, self.FLASH_KEY2)
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 1.0)
                self.mem_write32(SR, 0x3FB)
                self.mem_write32(CR, 1 << 2)             # MER1
                self.mem_write32(CR, (1 << 2) | (1 << 16))
                self._wait_flash_ready(SR, 1 << 16, 0x3FA, 1, 10.0)
                self.mem_write32(CR, 1 << 31)
            else:
                raise RuntimeError(f"Chua ho tro Mass Erase cho {info['name']}")
            return info
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def _read_core_reg(self, reg_idx: int) -> int:
        self.mem_write32(0xE000EDF4, reg_idx & 0x1F)  # DCRSR
        for _ in range(30):
            if self.mem_read32(0xE000EDF0) & (1 << 16):  # S_REGRDY
                return self.mem_read32(0xE000EDF8)       # DCRDR
        raise TimeoutError(f"DCRSR reg {reg_idx} timeout")

    def read_cpu_status(self, clock_hz: int = 500_000, connect_under_reset: bool = False) -> dict:
        try:
            dp_id = self._attach_and_halt(clock_hz, connect_under_reset)
            info = self._identify_halted_target(dp_id)
            regs = {
                "R0": self._read_core_reg(0),
                "R1": self._read_core_reg(1),
                "R2": self._read_core_reg(2),
                "R3": self._read_core_reg(3),
                "R12": self._read_core_reg(12),
                "SP": self._read_core_reg(13),
                "LR": self._read_core_reg(14),
                "PC": self._read_core_reg(15),
                "xPSR": self._read_core_reg(16),
                "MSP": self._read_core_reg(17),
                "PSP": self._read_core_reg(18),
            }
            cfsr = self.mem_read32(0xE000ED28)
            hfsr = self.mem_read32(0xE000ED2C)
            return {"info": info, "regs": regs, "cfsr": cfsr, "hfsr": hfsr}
        finally:
            try:
                self.disconnect()
            except Exception:
                pass

    def cpu_control(self, action: str, clock_hz: int = 500_000) -> str:
        if self.connect_swd() != 1:
            raise RuntimeError("DAP_Connect(SWD) failed")
        try:
            self.swj_clock(clock_hz)
            self.transfer_configure(2, 500, 0)
            self.swd_configure(0)
            self.swd_line_reset_to_swd()
            self.read_dp(0x00)
            self.write_abort(0x1E)
            self.write_dp(0x08, 0)
            self.write_dp(0x04, 0x50000000)
            for _ in range(30):
                if (self.read_dp(0x04) & 0xA0000000) == 0xA0000000:
                    break
                time.sleep(0.002)
            self.transfer([(0x01 | 0x00, 0x23000012)])

            if action == "halt":
                self.mem_write32(0xE000EDF0, 0xA05F0003)
                return "CPU HALTED"
            elif action == "run":
                self.mem_write32(0xE000EDF0, 0xA05F0001)
                return "CPU RUNNING"
            elif action == "step":
                self.mem_write32(0xE000EDF0, 0xA05F0003)
                self.mem_write32(0xE000EDF0, 0xA05F0005)  # C_STEP | C_DEBUGEN
                pc = self._read_core_reg(15)
                return f"CPU STEP -> PC=0x{pc:08X}"
            else:
                raise ValueError(action)
        finally:
            try:
                self.disconnect()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Tkinter GUI Application
# ---------------------------------------------------------------------------
class TueDapGui(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TUE CMSIS-DAP — Nap & Debug STM32 Da Dong (F1 / F3 / F4 / G4 / L4)")
        self.geometry("950x740")
        self.minsize(850, 620)

        self.hid_devices = []
        self.serial_port = None
        self.serial_stop = threading.Event()
        self.openocd_proc = None
        self.log_queue = queue.Queue()

        self._build_ui()
        self.refresh_hid_devices()
        self.refresh_com_ports()
        self.after(80, self._poll_logs)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self):
        # Top Probe Bar
        top = ttk.LabelFrame(self, text="1. Thiet bi USB CMSIS-DAP (Driverless Windows HID)", padding=8)
        top.pack(fill="x", padx=10, pady=(10, 5))

        ttk.Label(top, text="Thiet bi:").pack(side="left")
        self.dev_var = tk.StringVar()
        self.dev_combo = ttk.Combobox(top, textvariable=self.dev_var, state="readonly", width=52)
        self.dev_combo.pack(side="left", padx=6, fill="x", expand=True)

        ttk.Button(top, text="Quet lai USB", command=self.refresh_hid_devices).pack(side="left", padx=3)
        ttk.Button(top, text="Test DAP_Info", command=self.test_dap_info).pack(side="left", padx=3)

        # Notebook tabs
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=False, padx=10, pady=5)

        tab_swd = ttk.Frame(nb, padding=10)
        tab_mem = ttk.Frame(nb, padding=10)
        tab_ocd = ttk.Frame(nb, padding=10)
        tab_uart = ttk.Frame(nb, padding=10)
        nb.add(tab_swd, text="Nap Code & Probe Truc Tiep (Khong can OpenOCD)")
        nb.add(tab_mem, text="Doc Flash / Hex Viewer & Debug CPU")
        nb.add(tab_ocd, text="OpenOCD & GDB Debug Server (Tuy chon)")
        nb.add(tab_uart, text="UART CLI (USART1 115200)")

        self._build_swd_tab(tab_swd)
        self._build_mem_tab(tab_mem)
        self._build_ocd_tab(tab_ocd)
        self._build_uart_tab(tab_uart)

        # Log Console
        log_frame = ttk.LabelFrame(self, text="Log ket qua", padding=6)
        log_frame.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        self.log_box = scrolledtext.ScrolledText(log_frame, height=13, font=("Consolas", 10), state="disabled")
        self.log_box.pack(fill="both", expand=True)
        ttk.Button(log_frame, text="Xoa log", command=self._clear_log).pack(anchor="e", pady=(4, 0))

    def _build_swd_tab(self, parent):
        row1 = ttk.Frame(parent)
        row1.pack(fill="x", pady=4)
        ttk.Label(row1, text="SWD Clock:").pack(side="left")
        self.clk_var = tk.StringVar(value="500000")
        ttk.Combobox(
            row1,
            textvariable=self.clk_var,
            values=["100000", "500000", "1000000", "2000000", "4000000"],
            width=10,
            state="readonly",
        ).pack(side="left", padx=5)
        ttk.Label(row1, text="Hz").pack(side="left", padx=(0, 8))

        self.cur_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(row1, text="Connect Under Reset (PB8)", variable=self.cur_var).pack(side="left", padx=4)
        ttk.Button(row1, text="Probe Target", command=self.probe_target).pack(side="left", padx=4)
        ttk.Button(row1, text="Reset Target (PB8)", command=self.hard_reset_target).pack(side="left", padx=4)
        ttk.Button(row1, text="Toggle LED (PC13)", command=self.toggle_probe_led).pack(side="left", padx=4)

        # Direct Flash section (NO OpenOCD!)
        flash_box = ttk.LabelFrame(parent, text="Nap Code Truc Tiep qua USB HID (Ho tro F1 / F3 / F4 / G4 / L4)", padding=8)
        flash_box.pack(fill="x", pady=8)

        fr1 = ttk.Frame(flash_box)
        fr1.pack(fill="x", pady=3)
        ttk.Label(fr1, text="File (.bin / .hex / .elf):").pack(side="left")
        self.direct_fw_var = tk.StringVar()
        ttk.Entry(fr1, textvariable=self.direct_fw_var, width=48).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(fr1, text="Chon file...", command=self._browse_direct_fw).pack(side="left", padx=3)
        ttk.Label(fr1, text="Addr (.bin):").pack(side="left", padx=(6, 2))
        self.direct_addr_var = tk.StringVar(value="0x08000000")
        ttk.Entry(fr1, textvariable=self.direct_addr_var, width=12).pack(side="left")

        fr2 = ttk.Frame(flash_box)
        fr2.pack(fill="x", pady=(6, 2))
        self.direct_verify_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(fr2, text="Verify sau nap", variable=self.direct_verify_var).pack(side="left")
        self.direct_flash_btn = ttk.Button(fr2, text="NAP CODE TRUC TIEP", command=self.direct_flash)
        self.direct_flash_btn.pack(side="left", padx=6)
        ttk.Button(fr2, text="So sanh (Verify)", command=self.direct_verify_only).pack(side="left", padx=3)
        ttk.Button(fr2, text="Full Chip Erase", command=self.direct_mass_erase).pack(side="left", padx=3)
        self.prog_var = tk.DoubleVar(value=0.0)
        self.prog_bar = ttk.Progressbar(fr2, variable=self.prog_var, maximum=100, length=180)
        self.prog_bar.pack(side="left", padx=6, fill="x", expand=True)
        self.prog_lbl = ttk.Label(fr2, text="San sang", width=16)
        self.prog_lbl.pack(side="left")

        row2 = ttk.Frame(parent)
        row2.pack(fill="x", pady=4)
        ttk.Label(row2, text="Doc 32-bit tai dia chi:").pack(side="left")
        self.addr_var = tk.StringVar(value="0xE0042000")
        ttk.Entry(row2, textvariable=self.addr_var, width=14).pack(side="left", padx=6)
        ttk.Button(row2, text="Read Mem32", command=self.read_mem32).pack(side="left", padx=4)
        ttk.Button(row2, text="Doc Flash Base (0x08000000)", command=lambda: self.read_mem32("0x08000000")).pack(side="left", padx=4)

        self.target_lbl = ttk.Label(parent, text="Trang thai Target: Chua kiem tra", font=("Segoe UI", 9, "bold"))
        self.target_lbl.pack(anchor="w", pady=(4, 0))

    def _build_mem_tab(self, parent):
        cpu_row = ttk.LabelFrame(parent, text="Dieu khien CPU & Doc Thanh Ghi / HardFault / UID", padding=6)
        cpu_row.pack(fill="x", pady=(0, 6))
        ttk.Button(cpu_row, text="Halt CPU", command=lambda: self.cpu_action("halt")).pack(side="left", padx=3)
        ttk.Button(cpu_row, text="Run CPU", command=lambda: self.cpu_action("run")).pack(side="left", padx=3)
        ttk.Button(cpu_row, text="Step 1 lenh", command=lambda: self.cpu_action("step")).pack(side="left", padx=3)
        ttk.Button(cpu_row, text="Doc CPU Regs (PC/SP/HardFault/UID)", command=self.read_cpu_regs).pack(side="left", padx=8)

        mem_row = ttk.Frame(parent)
        mem_row.pack(fill="x", pady=4)
        ttk.Label(mem_row, text="Dia chi:").pack(side="left")
        self.hex_addr_var = tk.StringVar(value="0x08000000")
        ttk.Entry(mem_row, textvariable=self.hex_addr_var, width=13).pack(side="left", padx=4)
        ttk.Label(mem_row, text="So byte:").pack(side="left")
        self.hex_len_var = tk.StringVar(value="512")
        ttk.Combobox(
            mem_row,
            textvariable=self.hex_len_var,
            values=["256", "512", "1024", "4096", "16384"],
            width=8,
        ).pack(side="left", padx=4)
        ttk.Button(mem_row, text="Doc Hex View", command=self.read_hex_view).pack(side="left", padx=4)
        ttk.Button(mem_row, text="Dump vung nay (.bin)", command=lambda: self.dump_flash_bin(full=False)).pack(side="left", padx=4)
        ttk.Button(mem_row, text="Dump Toan Bo Flash (.bin)", command=lambda: self.dump_flash_bin(full=True)).pack(side="left", padx=4)

        self.hex_box = scrolledtext.ScrolledText(parent, height=9, font=("Consolas", 9), state="disabled")
        self.hex_box.pack(fill="both", expand=True, pady=(4, 0))

    def _build_ocd_tab(self, parent):
        r1 = ttk.Frame(parent)
        r1.pack(fill="x", pady=4)
        ttk.Label(r1, text="OpenOCD exe:").pack(side="left")
        default_ocd = shutil.which("openocd") or (r"C:\msys64\mingw64\bin\openocd.exe" if os.name == "nt" else "/usr/bin/openocd")
        self.ocd_path_var = tk.StringVar(value=default_ocd)
        ttk.Entry(r1, textvariable=self.ocd_path_var, width=48).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(r1, text="Chon...", command=self._browse_ocd).pack(side="left")

        r2 = ttk.Frame(parent)
        r2.pack(fill="x", pady=4)
        ttk.Label(r2, text="Dong chip dich:").pack(side="left")
        self.ocd_target_var = tk.StringVar(value=OPENOCD_TARGETS[0][0])
        ttk.Combobox(
            r2,
            textvariable=self.ocd_target_var,
            values=[t[0] for t in OPENOCD_TARGETS],
            state="readonly",
            width=30,
        ).pack(side="left", padx=6)
        self.ocd_cur_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(r2, text="Connect Under Reset (PB8)", variable=self.ocd_cur_var).pack(side="left", padx=6)
        ttk.Button(r2, text="Test ket noi OpenOCD", command=self.ocd_test_connect).pack(side="left", padx=6)

        r3 = ttk.Frame(parent)
        r3.pack(fill="x", pady=4)
        ttk.Label(r3, text="File nap (.bin/.elf/.hex):").pack(side="left")
        self.fw_path_var = tk.StringVar()
        ttk.Entry(r3, textvariable=self.fw_path_var, width=44).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(r3, text="Chon file...", command=self._browse_fw).pack(side="left", padx=3)
        ttk.Label(r3, text="Addr (.bin):").pack(side="left", padx=(6, 2))
        self.bin_addr_var = tk.StringVar(value="0x08000000")
        ttk.Entry(r3, textvariable=self.bin_addr_var, width=12).pack(side="left")

        r4 = ttk.Frame(parent)
        r4.pack(fill="x", pady=8)
        ttk.Button(r4, text="Nap Code + Verify + Reset (OpenOCD)", command=self.ocd_flash).pack(side="left", padx=4)
        self.gdb_btn = ttk.Button(r4, text="Bat GDB Server (:3333)", command=self.ocd_toggle_gdb)
        self.gdb_btn.pack(side="left", padx=4)

    def _build_uart_tab(self, parent):
        r1 = ttk.Frame(parent)
        r1.pack(fill="x", pady=4)
        ttk.Label(r1, text="Cong COM:").pack(side="left")
        self.com_var = tk.StringVar()
        self.com_combo = ttk.Combobox(r1, textvariable=self.com_var, state="readonly", width=35)
        self.com_combo.pack(side="left", padx=6)
        ttk.Button(r1, text="Quet COM", command=self.refresh_com_ports).pack(side="left", padx=3)
        self.com_btn = ttk.Button(r1, text="Mo COM (115200)", command=self.toggle_serial)
        self.com_btn.pack(side="left", padx=4)

        r2 = ttk.Frame(parent)
        r2.pack(fill="x", pady=6)
        for label, cmd in [("Probe", "probe"), ("Link", "link"), ("Reset", "reset"), ("Halt", "halt"), ("Run", "run"), ("Status", "status")]:
            ttk.Button(r2, text=label, command=lambda c=cmd: self.send_serial_cmd(c)).pack(side="left", padx=3)

        r3 = ttk.Frame(parent)
        r3.pack(fill="x", pady=4)
        self.uart_cmd_var = tk.StringVar()
        ent = ttk.Entry(r3, textvariable=self.uart_cmd_var, width=50)
        ent.pack(side="left", padx=(0, 6), fill="x", expand=True)
        ent.bind("<Return>", lambda e: self.send_serial_cmd(self.uart_cmd_var.get(), clear_entry=True))
        ttk.Button(r3, text="Gui lenh", command=lambda: self.send_serial_cmd(self.uart_cmd_var.get(), clear_entry=True)).pack(side="left")

    # -----------------------------------------------------------------------
    # Logging & Helpers
    # -----------------------------------------------------------------------
    def log(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        self.log_queue.put(f"[{ts}] {msg}\n")

    def _poll_logs(self):
        while not self.log_queue.empty():
            line = self.log_queue.get_nowait()
            self.log_box.configure(state="normal")
            self.log_box.insert("end", line)
            self.log_box.see("end")
            self.log_box.configure(state="disabled")
        self.after(80, self._poll_logs)

    def _clear_log(self):
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")

    def _selected_hid(self):
        idx = self.dev_combo.current()
        if idx < 0 or idx >= len(self.hid_devices):
            raise RuntimeError("Chua chon thiet bi USB HID nao (hay bam 'Quet lai USB')")
        return self.hid_devices[idx]

    def _run_bg(self, fn):
        threading.Thread(target=fn, daemon=True).start()

    # -----------------------------------------------------------------------
    # Direct USB HID / CMSIS-DAP Actions
    # -----------------------------------------------------------------------
    def refresh_hid_devices(self):
        all_devs = enumerate_hid_devices()
        filtered = [
            d for d in all_devs
            if ("CMSIS-DAP" in d["prod"]) or (d["vid"] == 0x0483) or (d["usage_page"] == 0xFF00)
        ]
        self.hid_devices = filtered
        labels = [
            f"{d['prod'] or 'HID Device'} (VID={d['vid']:04X} PID={d['pid']:04X} IN={d['in_len']}B OUT={d['out_len']}B)"
            for d in filtered
        ]
        self.dev_combo["values"] = labels
        if labels:
            self.dev_combo.current(0)
            self.log(f"Tim thay {len(labels)} thiet bi HID: {labels[0]}")
        else:
            self.dev_var.set("")
            self.log("Khong tim thay thiet bi CMSIS-DAP / VID_0483. Hay kiem tra day USB & BOOT0=0.")

    def test_dap_info(self):
        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    dap = CmsisDapClient(dev)
                    vendor = dap.info_str(0x01)
                    prod = dap.info_str(0x02)
                    ser = dap.info_str(0x03)
                    fw = dap.info_str(0x04)
                    caps = dap.info_bytes(0xF0)
                    ts_clk = struct.unpack("<I", dap.info_bytes(0xF1))[0]
                    pkt_cnt = dap.info_bytes(0xFE)[0]
                    pkt_sz = struct.unpack("<H", dap.info_bytes(0xFF))[0]
                    self.log(
                        f"DAP_Info OK -> Vendor='{vendor}' | Product='{prod}' | FW={fw} | "
                        f"SN={ser} | Caps=0x{caps.hex()} | Clk={ts_clk // 1_000_000}MHz | Pkt={pkt_cnt}x{pkt_sz}B"
                    )
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI test DAP_Info: {e}")
        self._run_bg(work)

    def toggle_probe_led(self):
        self._led_state = not getattr(self, "_led_state", False)
        state = self._led_state

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    CmsisDapClient(dev).host_status(0, state)
                    self.log(f"LED PC13 -> {'BAT' if state else 'TAT'}")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI LED: {e}")
        self._run_bg(work)

    def hard_reset_target(self):
        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    dap = CmsisDapClient(dev)
                    dap.connect_swd()
                    dap.reset_target()
                    dap.disconnect()
                    self.log("Da kich xung Hard Reset (PB8 NRST) toi Target.")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Reset: {e}")
        self._run_bg(work)

    def probe_target(self):
        def work():
            try:
                d = self._selected_hid()
                clk = int(self.clk_var.get())
                cur = self.cur_var.get()
                dev = open_hid_device(d)
                try:
                    info = CmsisDapClient(dev).probe_target(clk, cur)
                    rdp_str = f"0x{info['rdp']:02X} ({'Level 0' if info['rdp'] == 0xAA else 'Protected'})" if info["rdp"] is not None else "N/A"
                    summary = (
                        f"Target: {info['name']} | DEV_ID=0x{info['dev_id']:03X} | "
                        f"Flash={info['flash_kb']}KB | UID={info['uid']} | RDP={rdp_str}"
                    )
                    self.log("PROBE OK -> " + summary)
                    self.after(0, lambda: self.target_lbl.config(text="Trang thai Target: " + summary))
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"PROBE THAT BAI: {e}")
                self.after(0, lambda: self.target_lbl.config(text=f"Trang thai Target: Loi ({e})"))
        self._run_bg(work)

    def _browse_direct_fw(self):
        p = filedialog.askopenfilename(title="Chon file firmware", filetypes=[("Firmware", "*.bin *.hex *.elf"), ("All", "*.*")])
        if p:
            self.direct_fw_var.set(p)
            self.fw_path_var.set(p)

    def direct_flash(self):
        fw_path = self.direct_fw_var.get().strip()
        if not fw_path or not os.path.exists(fw_path):
            messagebox.showwarning("Thieu file", "Vui long chon file .bin / .hex / .elf hop le!")
            return
        try:
            bin_addr = int(self.direct_addr_var.get().strip() or "0x08000000", 0)
            base_addr, payload = load_firmware_image(fw_path, bin_addr)
        except Exception as e:
            messagebox.showerror("Loi doc file", str(e))
            return

        clk = int(self.clk_var.get())
        cur = self.cur_var.get()
        verify = self.direct_verify_var.get()
        self.direct_flash_btn.config(state="disabled")
        self.prog_var.set(0)
        self.prog_lbl.config(text="Dang ket noi...")

        def on_progress(stage, done, total):
            pct = (done * 100.0 / total) if total > 0 else 100.0
            self.after(0, lambda: (
                self.prog_var.set(pct),
                self.prog_lbl.config(text=f"{stage}: {pct:.0f}%"),
            ))

        def work():
            t0 = time.time()
            try:
                d = self._selected_hid()
                self.log(f"Bat dau nap truc tiep {len(payload)} B @ 0x{base_addr:08X} ({os.path.basename(fw_path)})...")
                dev = open_hid_device(d)
                try:
                    info = CmsisDapClient(dev).flash_image(base_addr, payload, clk, cur, verify, on_progress)
                    dt = time.time() - t0
                    self.log(f"NAP THANH CONG {info['name']} ({len(payload)} B trong {dt:.2f}s) -> Da Verify & Reset!")
                    self.after(0, lambda: (
                        self.prog_var.set(100),
                        self.prog_lbl.config(text=f"Xong ({dt:.1f}s)"),
                        self.target_lbl.config(text=f"Trang thai Target: {info['name']} | Da nap {len(payload)} B OK"),
                    ))
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"NAP THAT BAI: {e}")
                self.after(0, lambda: self.prog_lbl.config(text="Loi nap!"))
            finally:
                self.after(0, lambda: self.direct_flash_btn.config(state="normal"))

        self._run_bg(work)

    def direct_verify_only(self):
        fw_path = self.direct_fw_var.get().strip()
        if not fw_path or not os.path.exists(fw_path):
            messagebox.showwarning("Thieu file", "Vui long chon file .bin / .hex / .elf de so sanh!")
            return
        try:
            bin_addr = int(self.direct_addr_var.get().strip() or "0x08000000", 0)
            base_addr, payload = load_firmware_image(fw_path, bin_addr)
        except Exception as e:
            messagebox.showerror("Loi doc file", str(e))
            return

        clk = int(self.clk_var.get())
        cur = self.cur_var.get()

        def on_progress(stage, done, total):
            pct = (done * 100.0 / total) if total > 0 else 100.0
            self.after(0, lambda: (self.prog_var.set(pct), self.prog_lbl.config(text=f"{stage}: {pct:.0f}%")))

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    info, mismatches, first_bad = CmsisDapClient(dev).verify_image(base_addr, payload, clk, cur, on_progress)
                    if mismatches == 0:
                        self.log(f"VERIFY KHOP 100% ({len(payload)} B) voi {info['name']}!")
                        self.after(0, lambda: self.prog_lbl.config(text="Verify OK 100%"))
                    else:
                        addr, want, got = first_bad
                        self.log(f"VERIFY KHONG KHOP: {mismatches} word sai (dau tien @ 0x{addr:08X}: file=0x{want:08X} chip=0x{got:08X})")
                        self.after(0, lambda: self.prog_lbl.config(text=f"Sai {mismatches} word"))
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Verify: {e}")

        self._run_bg(work)

    def direct_mass_erase(self):
        if not messagebox.askyesno("Xac nhan xoa chip", "Xoa trang toan bo Flash cua chip dich (Full Chip Erase)?"):
            return
        clk = int(self.clk_var.get())
        cur = self.cur_var.get()

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    info = CmsisDapClient(dev).mass_erase(clk, cur)
                    self.log(f"FULL CHIP ERASE THANH CONG: {info['name']} ({info['flash_kb']} KB)")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Mass Erase: {e}")

        self._run_bg(work)

    def cpu_action(self, action: str):
        clk = int(self.clk_var.get())

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    msg = CmsisDapClient(dev).cpu_control(action, clk)
                    self.log(msg)
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI CPU {action}: {e}")

        self._run_bg(work)

    def read_cpu_regs(self):
        clk = int(self.clk_var.get())
        cur = self.cur_var.get()

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    res = CmsisDapClient(dev).read_cpu_status(clk, cur)
                    info, regs = res["info"], res["regs"]
                    self.log(f"=== {info['name']} | UID: {info['uid']} | Flash: {info['flash_kb']} KB ===")
                    self.log(
                        f"PC=0x{regs['PC']:08X}  SP=0x{regs['SP']:08X}  LR=0x{regs['LR']:08X}  xPSR=0x{regs['xPSR']:08X}"
                    )
                    self.log(
                        f"R0=0x{regs['R0']:08X}  R1=0x{regs['R1']:08X}  R2=0x{regs['R2']:08X}  R3=0x{regs['R3']:08X}  R12=0x{regs['R12']:08X}"
                    )
                    self.log(f"MSP=0x{regs['MSP']:08X}  PSP=0x{regs['PSP']:08X}  CFSR=0x{res['cfsr']:08X}  HFSR=0x{res['hfsr']:08X}")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI doc CPU Regs: {e}")

        self._run_bg(work)

    def read_hex_view(self):
        try:
            addr = int(self.hex_addr_var.get().strip(), 0)
            length = int(self.hex_len_var.get().strip(), 0)
            if length <= 0 or length > 65536:
                raise ValueError("Kich thuoc xem Hex tu 1..65536 bytes")
        except Exception as e:
            messagebox.showerror("Tham so sai", str(e))
            return

        clk = int(self.clk_var.get())
        cur = self.cur_var.get()

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    data = CmsisDapClient(dev).read_memory_bytes(addr, length, clk, cur)
                    lines = []
                    for off in range(0, len(data), 16):
                        chunk = data[off : off + 16]
                        hex_part = " ".join(f"{b:02X}" for b in chunk)
                        asc_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                        lines.append(f"{addr + off:08X}  {hex_part:<47}  |{asc_part}|")
                    text = "\n".join(lines)

                    def update_box():
                        self.hex_box.configure(state="normal")
                        self.hex_box.delete("1.0", "end")
                        self.hex_box.insert("1.0", text)
                        self.hex_box.configure(state="disabled")

                    self.after(0, update_box)
                    self.log(f"Da doc {len(data)} bytes tu 0x{addr:08X} vao Hex Viewer.")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI doc Hex: {e}")

        self._run_bg(work)

    def dump_flash_bin(self, full: bool = False):
        save_path = filedialog.asksaveasfilename(
            title="Luu file dump .bin",
            defaultextension=".bin",
            filetypes=[("Binary file", "*.bin"), ("All", "*.*")],
        )
        if not save_path:
            return
        clk = int(self.clk_var.get())
        cur = self.cur_var.get()

        def on_progress(stage, done, total):
            pct = (done * 100.0 / total) if total > 0 else 100.0
            self.after(0, lambda: (self.prog_var.set(pct), self.prog_lbl.config(text=f"{stage}: {pct:.0f}%")))

        def work():
            try:
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    dap = CmsisDapClient(dev)
                    if full:
                        info = dap.probe_target(clk, cur)
                        if info["flash_kb"] <= 0:
                            raise RuntimeError("Khong doc duoc dung luong Flash cua chip")
                        addr = 0x08000000
                        length = info["flash_kb"] * 1024
                    else:
                        addr = int(self.hex_addr_var.get().strip(), 0)
                        length = int(self.hex_len_var.get().strip(), 0)

                    self.log(f"Dang dump {length} bytes tu 0x{addr:08X}...")
                    data = dap.read_memory_bytes(addr, length, clk, cur, on_progress)
                    with open(save_path, "wb") as f:
                        f.write(data)
                    self.log(f"DUMP THANH CONG {len(data)} bytes -> {save_path}")
                    self.after(0, lambda: self.prog_lbl.config(text="Dump xong!"))
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Dump: {e}")

        self._run_bg(work)

    def read_mem32(self, override_addr=None):
        def work():
            try:
                addr_str = override_addr or self.addr_var.get().strip()
                addr = int(addr_str, 0)
                clk = int(self.clk_var.get())
                cur = self.cur_var.get()
                d = self._selected_hid()
                dev = open_hid_device(d)
                try:
                    dap = CmsisDapClient(dev)
                    dap._attach_and_halt(clk, cur)
                    val = dap.mem_read32(addr)
                    dap.mem_write32(0xE000EDF0, 0xA05F0001)
                    dap.disconnect()
                    self.log(f"MEM[0x{addr:08X}] = 0x{val:08X}")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Read Mem32: {e}")
        self._run_bg(work)

    # -----------------------------------------------------------------------
    # OpenOCD Actions (Optional)
    # -----------------------------------------------------------------------
    def _browse_ocd(self):
        p = filedialog.askopenfilename(title="Chon openocd.exe", filetypes=[("Executable", "*.exe"), ("All", "*.*")])
        if p:
            self.ocd_path_var.set(p)

    def _browse_fw(self):
        p = filedialog.askopenfilename(title="Chon file firmware", filetypes=[("Firmware", "*.bin *.elf *.hex"), ("All", "*.*")])
        if p:
            self.fw_path_var.set(p)

    def _get_target_cfg(self) -> str:
        sel = self.ocd_target_var.get()
        for label, cfg in OPENOCD_TARGETS:
            if label == sel:
                return cfg
        return "target/stm32f4x.cfg"

    def _run_openocd_cmd(self, extra_cmds):
        ocd = self.ocd_path_var.get().strip()
        if not ocd or not os.path.exists(ocd):
            self.log(f"Khong tim thay openocd.exe tai: {ocd}")
            return
        cfg = self._get_target_cfg()
        cmd = [ocd, "-f", "interface/cmsis-dap.cfg", "-f", cfg, "-c", f"adapter speed {int(self.clk_var.get()) // 1000}"]
        if self.ocd_cur_var.get():
            cmd += ["-c", "reset_config srst_only connect_assert_srst"]
        for c in extra_cmds:
            cmd += ["-c", c]
        self.log("Chay: " + " ".join(cmd))

        def work():
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
                for line in proc.stdout:
                    self.log("[OpenOCD] " + line.rstrip())
                rc = proc.wait()
                self.log(f"OpenOCD ket thuc (exit code = {rc})")
            except Exception as e:
                self.log(f"LOI OpenOCD: {e}")
        self._run_bg(work)

    def ocd_test_connect(self):
        self._run_openocd_cmd(["init", "targets", "exit"])

    def ocd_flash(self):
        fw = self.fw_path_var.get().strip().replace("\\", "/")
        if not fw or not os.path.exists(fw):
            messagebox.showwarning("Thieu file", "Vui long chon file .bin / .elf / .hex hop le!")
            return
        if fw.lower().endswith(".bin"):
            addr = self.bin_addr_var.get().strip() or "0x08000000"
            prog_cmd = f'program "{fw}" {addr} verify reset exit'
        else:
            prog_cmd = f'program "{fw}" verify reset exit'
        self._run_openocd_cmd([prog_cmd])

    def ocd_toggle_gdb(self):
        if self.openocd_proc and self.openocd_proc.poll() is None:
            self.openocd_proc.terminate()
            self.openocd_proc = None
            self.gdb_btn.config(text="Bat GDB Server (:3333)")
            self.log("Da tat OpenOCD GDB Server.")
            return

        ocd = self.ocd_path_var.get().strip()
        if not ocd or not os.path.exists(ocd):
            self.log(f"Khong tim thay openocd.exe tai: {ocd}")
            return
        cfg = self._get_target_cfg()
        cmd = [ocd, "-f", "interface/cmsis-dap.cfg", "-f", cfg, "-c", f"adapter speed {int(self.clk_var.get()) // 1000}"]
        if self.ocd_cur_var.get():
            cmd += ["-c", "reset_config srst_only connect_assert_srst"]
        self.log("Khoi dong GDB Server (:3333): " + " ".join(cmd))
        self.gdb_btn.config(text="Tat GDB Server (:3333)")

        def work():
            try:
                self.openocd_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
                for line in self.openocd_proc.stdout:
                    self.log("[GDB-OCD] " + line.rstrip())
            except Exception as e:
                self.log(f"LOI GDB Server: {e}")
            finally:
                self.after(0, lambda: self.gdb_btn.config(text="Bat GDB Server (:3333)"))
        self._run_bg(work)

    # -----------------------------------------------------------------------
    # UART CLI Actions
    # -----------------------------------------------------------------------
    def refresh_com_ports(self):
        if not HAS_SERIAL:
            self.com_combo["values"] = ["(Chua cai pyserial)"]
            return
        ports = [f"{p.device} - {p.description}" for p in serial.tools.list_ports.comports()]
        self.com_combo["values"] = ports
        if ports:
            self.com_combo.current(0)

    def toggle_serial(self):
        if not HAS_SERIAL:
            messagebox.showerror("Thieu pyserial", "Hay cai dat: pip install pyserial")
            return
        if self.serial_port and self.serial_port.is_open:
            self.serial_stop.set()
            self.serial_port.close()
            self.serial_port = None
            self.com_btn.config(text="Mo COM (115200)")
            self.log("Da dong cong COM.")
            return

        sel = self.com_var.get()
        if not sel:
            return
        port_name = sel.split(" - ")[0].strip()
        try:
            self.serial_port = serial.Serial(port_name, 115200, timeout=0.1)
            self.serial_stop.clear()
            self.com_btn.config(text="Dong COM")
            self.log(f"Da mo {port_name} @ 115200 baud.")

            def reader():
                while not self.serial_stop.is_set() and self.serial_port and self.serial_port.is_open:
                    try:
                        line = self.serial_port.readline()
                        if line:
                            self.log("[UART] " + line.decode("utf-8", errors="replace").rstrip())
                    except Exception:
                        break
            self._run_bg(reader)
        except Exception as e:
            self.log(f"LOI mo {port_name}: {e}")

    def send_serial_cmd(self, cmd: str, clear_entry: bool = False):
        if not self.serial_port or not self.serial_port.is_open:
            self.log("Chua mo cong COM trong tab UART CLI.")
            return
        cmd = cmd.strip()
        if not cmd:
            return
        self.serial_port.write((cmd + "\r\n").encode("utf-8"))
        self.log(f"[UART TX] {cmd}")
        if clear_entry:
            self.uart_cmd_var.set("")

    def _on_close(self):
        self.serial_stop.set()
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        if self.openocd_proc and self.openocd_proc.poll() is None:
            self.openocd_proc.terminate()
        self.destroy()


if __name__ == "__main__":
    app = TueDapGui()
    app.mainloop()
