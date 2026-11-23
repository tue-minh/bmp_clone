"""
TUE CMSIS-DAP & Multi-STM32 SWD Test GUI
Zero external dependencies for USB HID (uses Windows hid.dll/setupapi.dll via ctypes)
+ pyserial (optional, for USART1 CLI) + OpenOCD integration (for flashing F1/F4/G4...).
"""

import ctypes
import ctypes.wintypes as wt
import os
import queue
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
# STM32 Device ID lookup (matches stm32_target.c)
# ---------------------------------------------------------------------------
STM32_DEVICES = {
    0x412: ("F1", "STM32F10x Low-density"),
    0x410: ("F1", "STM32F10x Medium-density (F103C8/RB)"),
    0x414: ("F1", "STM32F10x High-density (F103RC/ZE)"),
    0x418: ("F1", "STM32F105/F107 Connectivity"),
    0x420: ("F1", "STM32F100 Value Line"),
    0x422: ("F3", "STM32F302/F303xB/C"),
    0x438: ("F3", "STM32F303x6/8 / F334"),
    0x411: ("F2", "STM32F20x/F21x"),
    0x413: ("F4", "STM32F405/F407/F415/F417"),
    0x419: ("F4", "STM32F427/F429/F437/F439"),
    0x421: ("F4", "STM32F446"),
    0x423: ("F4", "STM32F401xB/C"),
    0x431: ("F4", "STM32F411xC/E"),
    0x433: ("F4", "STM32F401xD/E"),
    0x441: ("F4", "STM32F412"),
    0x463: ("F4", "STM32F413/F423"),
    0x468: ("G4", "STM32G431/G441 (Cat.2)"),
    0x469: ("G4", "STM32G473/G474/G484 (Cat.3)"),
    0x479: ("G4", "STM32G491/G4A1 (Cat.4)"),
    0x415: ("L4", "STM32L475/L476/L486"),
    0x435: ("L4", "STM32L43x/L44x"),
    0x462: ("L4", "STM32L45x/L46x"),
    0x464: ("L4", "STM32L41x/L42x"),
    0x440: ("F0", "STM32F05x/F030x8"),
    0x444: ("F0", "STM32F03x/F04x"),
    0x460: ("G0", "STM32G07x/G08x"),
    0x466: ("G0", "STM32G03x/G04x"),
    0x467: ("G0", "STM32G0Bx/G0Cx"),
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
    hid_dll.HidD_GetSerialNumberString.argtypes = [wt.HANDLE, wt.LPWSTR, wt.ULONG]
    hid_dll.HidD_GetPreparsedData.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    hid_dll.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid_dll.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]


def enumerate_hid_devices():
    if os.name != "nt":
        return []
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
                    if hid_dll.HidP_GetCaps(ppd, ctypes.byref(caps)) == 0x00110000:  # HIDP_STATUS_SUCCESS
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

    def exchange(self, payload: bytes, timeout_ms: int = 1000) -> bytes:
        # Windows HID expects Report ID 0x00 prepended when descriptor has no Report ID
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


# ---------------------------------------------------------------------------
# CMSIS-DAP Protocol Client (talks to TUE CMSIS-DAP over USB HID)
# ---------------------------------------------------------------------------
class CmsisDapClient:
    ID_DAP_INFO = 0x00
    ID_DAP_HOST_STATUS = 0x01
    ID_DAP_CONNECT = 0x02
    ID_DAP_DISCONNECT = 0x03
    ID_DAP_TRANSFER_CONFIGURE = 0x04
    ID_DAP_TRANSFER = 0x05
    ID_DAP_WRITE_ABORT = 0x08
    ID_DAP_RESET_TARGET = 0x0A
    ID_DAP_SWJ_PINS = 0x10
    ID_DAP_SWJ_CLOCK = 0x11
    ID_DAP_SWJ_SEQUENCE = 0x12
    ID_DAP_SWD_CONFIGURE = 0x13

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
        if n == 0:
            return ""
        return r[2 : 2 + n].rstrip(b"\x00").decode("utf-8", errors="replace")

    def info_bytes(self, info_id: int) -> bytes:
        r = self.cmd(bytes([self.ID_DAP_INFO, info_id]))
        n = r[1]
        return r[2 : 2 + n]

    def host_status(self, led_type: int, on: bool):
        self.cmd(bytes([self.ID_DAP_HOST_STATUS, led_type, 1 if on else 0]))

    def connect_swd(self) -> int:
        r = self.cmd(bytes([self.ID_DAP_CONNECT, 0x01]))
        return r[1]

    def disconnect(self):
        self.cmd(bytes([self.ID_DAP_DISCONNECT]))

    def swj_clock(self, hz: int):
        self.cmd(bytes([self.ID_DAP_SWJ_CLOCK]) + struct.pack("<I", hz))

    def transfer_configure(self, idle=2, wait_retry=100, match_retry=0):
        self.cmd(bytes([self.ID_DAP_TRANSFER_CONFIGURE, idle]) + struct.pack("<HH", wait_retry, match_retry))

    def swd_configure(self, cfg=0):
        self.cmd(bytes([self.ID_DAP_SWD_CONFIGURE, cfg]))

    def swj_sequence(self, bit_count: int, data: bytes):
        cnt = 0 if bit_count >= 256 else bit_count
        self.cmd(bytes([self.ID_DAP_SWJ_SEQUENCE, cnt]) + data)

    def swj_pins(self, pin_out: int, pin_sel: int, wait_us: int = 0) -> int:
        r = self.cmd(bytes([self.ID_DAP_SWJ_PINS, pin_out, pin_sel]) + struct.pack("<I", wait_us))
        return r[1]

    def reset_target(self) -> int:
        r = self.cmd(bytes([self.ID_DAP_RESET_TARGET]))
        return r[1]

    def write_abort(self, val: int = 0x1E):
        self.cmd(bytes([self.ID_DAP_WRITE_ABORT, 0]) + struct.pack("<I", val))

    def transfer(self, ops):
        """ops: list of (req_byte, optional_u32_write_val)"""
        buf = bytearray([self.ID_DAP_TRANSFER, 0x00, len(ops)])
        read_count = 0
        for req_b, val in ops:
            buf.append(req_b)
            if (req_b & 0x02) == 0:  # Write
                buf += struct.pack("<I", val)
            else:
                read_count += 1
        r = self.cmd(bytes(buf))
        cnt = r[1]
        ack = r[2]
        vals = []
        off = 3
        while off + 4 <= len(r) and len(vals) < read_count:
            vals.append(struct.unpack_from("<I", r, off)[0])
            off += 4
        return cnt, ack, vals

    def swd_line_reset_to_swd(self):
        # 56 clocks high + 0xE79E (LSB first: 0x9E, 0xE7) + 56 clocks high + 4 idle
        self.swj_sequence(56, b"\xFF" * 7)
        self.swj_sequence(16, b"\x9E\xE7")
        self.swj_sequence(56, b"\xFF" * 7)
        self.swj_sequence(8, b"\x00")

    def read_dp(self, addr: int) -> int:
        req = 0x02 | (addr & 0x0C)  # RnW=1, APnDP=0
        _, ack, vals = self.transfer([(req, 0)])
        if ack != 1:
            raise RuntimeError(f"SWD DP read 0x{addr:02X} failed (ACK={ack})")
        return vals[0]

    def write_dp(self, addr: int, val: int):
        req = 0x00 | (addr & 0x0C)  # RnW=0, APnDP=0
        _, ack, _ = self.transfer([(req, val)])
        if ack != 1:
            raise RuntimeError(f"SWD DP write 0x{addr:02X} failed (ACK={ack})")

    def mem_read32(self, addr: int) -> int:
        # Write AP TAR (0x04), then Read AP DRW (0x0C)
        _, ack, vals = self.transfer([
            (0x01 | 0x04, addr),        # AP Write TAR
            (0x01 | 0x02 | 0x0C, 0),    # AP Read DRW
        ])
        if ack != 1:
            raise RuntimeError(f"SWD memRead32 0x{addr:08X} failed (ACK={ack})")
        return vals[0]

    def mem_write32(self, addr: int, val: int):
        _, ack, _ = self.transfer([
            (0x01 | 0x04, addr),   # AP Write TAR
            (0x01 | 0x0C, val),    # AP Write DRW
        ])
        if ack != 1:
            raise RuntimeError(f"SWD memWrite32 0x{addr:08X} failed (ACK={ack})")

    def probe_target(self, clock_hz: int = 1_000_000) -> dict:
        if self.connect_swd() != 1:
            raise RuntimeError("DAP_Connect(SWD) failed")
        try:
            self.swj_clock(clock_hz)
            self.transfer_configure(2, 100, 0)
            self.swd_configure(0)
            self.swd_line_reset_to_swd()
            dp_id = self.read_dp(0x00)
            self.write_abort(0x1E)
            self.write_dp(0x08, 0x00000000)  # DP_SELECT = 0
            self.write_dp(0x04, 0x50000000)  # CTRL/STAT power-up
            for _ in range(50):
                st = self.read_dp(0x04)
                if (st & 0xA0000000) == 0xA0000000:
                    break
                time.sleep(0.002)
            else:
                raise RuntimeError("Debug power-up timeout")

            # AP CSW = 32-bit auto-inc (0x23000012)
            _, ack, _ = self.transfer([(0x01 | 0x00, 0x23000012)])
            if ack != 1:
                raise RuntimeError(f"AP_CSW write failed (ACK={ack})")

            # Halt core via DHCSR (0xE000EDF0 = 0xA05F0003)
            self.mem_write32(0xE000EDF0, 0xA05F0003)

            # Read DBGMCU_IDCODE (M3/M4/M7 first, fallback M0/M0+)
            dbg_id = self.mem_read32(0xE0042000)
            if (dbg_id & 0xFFF) == 0:
                dbg_id = self.mem_read32(0x40015800)

            dev_id = dbg_id & 0xFFF
            rev_id = (dbg_id >> 16) & 0xFFFF
            fam, name = STM32_DEVICES.get(dev_id, ("?", "Unknown STM32 / Cortex-M"))

            flash_kb = 0
            rdp = None
            if fam in ("F1", "F3"):
                flash_kb = self.mem_read32(0x1FFFF7E0) & 0xFFFF
                obr = self.mem_read32(0x4002201C)
                rdp = 0xBB if (obr & 2) else 0xAA
            elif fam in ("F2", "F4"):
                flash_kb = (self.mem_read32(0x1FFF7A20) >> 16) & 0xFFFF
                optcr = self.mem_read32(0x40023C14)
                rdp = (optcr >> 8) & 0xFF
            elif fam in ("G4", "L4"):
                flash_kb = self.mem_read32(0x1FFF75E0) & 0xFFFF
                optr = self.mem_read32(0x40022020)
                rdp = optr & 0xFF

            # Resume core
            self.mem_write32(0xE000EDF0, 0xA05F0001)
            return {
                "dp_id": dp_id,
                "dbg_id": dbg_id,
                "dev_id": dev_id,
                "rev_id": rev_id,
                "family": fam,
                "name": name,
                "flash_kb": flash_kb,
                "rdp": rdp,
            }
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
        self.title("TUE CMSIS-DAP & Multi-STM32 SWD Tester (STM32F401RCT6)")
        self.geometry("920x680")
        self.minsize(820, 580)

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
        top = ttk.LabelFrame(self, text="1. Thiet bi USB CMSIS-DAP (HID)", padding=8)
        top.pack(fill="x", padx=10, pady=(10, 5))

        ttk.Label(top, text="Thiet bi:").pack(side="left")
        self.dev_var = tk.StringVar()
        self.dev_combo = ttk.Combobox(top, textvariable=self.dev_var, state="readonly", width=55)
        self.dev_combo.pack(side="left", padx=6, fill="x", expand=True)

        ttk.Button(top, text="Quet lai USB", command=self.refresh_hid_devices).pack(side="left", padx=3)
        ttk.Button(top, text="Test DAP_Info", command=self.test_dap_info).pack(side="left", padx=3)

        # Notebook tabs
        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=10, pady=5)

        tab_swd = ttk.Frame(nb, padding=10)
        tab_ocd = ttk.Frame(nb, padding=10)
        tab_uart = ttk.Frame(nb, padding=10)
        nb.add(tab_swd, text="SWD & Target Probe (Truc tiep USB)")
        nb.add(tab_ocd, text="Nap Code & Debug (OpenOCD)")
        nb.add(tab_uart, text="UART CLI (USART1 115200)")

        self._build_swd_tab(tab_swd)
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
            width=12,
            state="readonly",
        ).pack(side="left", padx=6)
        ttk.Label(row1, text="Hz").pack(side="left", padx=(0, 14))

        ttk.Button(row1, text="Probe Target (F1/F4/G4...)", command=self.probe_target).pack(side="left", padx=4)
        ttk.Button(row1, text="Hard Reset Target (PB8)", command=self.hard_reset_target).pack(side="left", padx=4)
        ttk.Button(row1, text="Toggle LED (PC13)", command=self.toggle_probe_led).pack(side="left", padx=4)

        row2 = ttk.Frame(parent)
        row2.pack(fill="x", pady=8)
        ttk.Label(row2, text="Doc 32-bit tai dia chi (hex):").pack(side="left")
        self.addr_var = tk.StringVar(value="0xE0042000")
        ttk.Entry(row2, textvariable=self.addr_var, width=16).pack(side="left", padx=6)
        ttk.Button(row2, text="Read Mem32", command=self.read_mem32).pack(side="left", padx=4)
        ttk.Button(row2, text="Doc Flash Base (0x08000000)", command=lambda: self.read_mem32("0x08000000")).pack(side="left", padx=4)

        self.target_lbl = ttk.Label(parent, text="Trang thai Target: Chua kiem tra", font=("Segoe UI", 10, "bold"))
        self.target_lbl.pack(anchor="w", pady=6)

    def _build_ocd_tab(self, parent):
        r1 = ttk.Frame(parent)
        r1.pack(fill="x", pady=4)
        ttk.Label(r1, text="OpenOCD exe:").pack(side="left")
        default_ocd = shutil.which("openocd") or r"C:\msys64\mingw64\bin\openocd.exe"
        self.ocd_path_var = tk.StringVar(value=default_ocd)
        ttk.Entry(r1, textvariable=self.ocd_path_var, width=48).pack(side="left", padx=6, fill="x", expand=True)
        ttk.Button(r1, text="Chon...", command=self._browse_ocd).pack(side="left")

        r2 = ttk.Frame(parent)
        r2.pack(fill="x", pady=4)
        ttk.Label(r2, text="Dong chip dich:").pack(side="left")
        self.ocd_target_var = tk.StringVar(value=OPENOCD_TARGETS[2][0])
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
        ttk.Button(r4, text="Nap Code + Verify + Reset", command=self.ocd_flash).pack(side="left", padx=4)
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
        ent.bind("<Return>", lambda e: self.send_serial_cmd(self.uart_cmd_var.get()))
        ttk.Button(r3, text="Gui lenh", command=lambda: self.send_serial_cmd(self.uart_cmd_var.get())).pack(side="left")

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
    # USB HID / CMSIS-DAP Actions
    # -----------------------------------------------------------------------
    def refresh_hid_devices(self):
        all_devs = enumerate_hid_devices()
        # Prioritize CMSIS-DAP / ST VID 0x0483 / vendor-defined 0xFF00
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
            self.log(f"Tim thay {len(labels)} thiet bi HID phu hop: {labels[0]}")
        else:
            self.dev_var.set("")
            self.log("Khong tim thay thiet bi CMSIS-DAP / VID_0483. Hay kiem tra day USB & BOOT0=0.")

    def test_dap_info(self):
        def work():
            try:
                d = self._selected_hid()
                dev = WinHidDevice(d["path"], d["in_len"], d["out_len"])
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
                        f"SN={ser} | Caps=0x{caps.hex()} | Clk={ts_clk // 1_000_000}MHz | "
                        f"Pkt={pkt_cnt}x{pkt_sz}B"
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
                dev = WinHidDevice(d["path"], d["in_len"], d["out_len"])
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
                dev = WinHidDevice(d["path"], d["in_len"], d["out_len"])
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
                dev = WinHidDevice(d["path"], d["in_len"], d["out_len"])
                try:
                    info = CmsisDapClient(dev).probe_target(clk)
                    rdp_str = f"0x{info['rdp']:02X} ({'Level 0' if info['rdp'] == 0xAA else 'Protected'})" if info["rdp"] is not None else "N/A"
                    summary = (
                        f"Target: {info['name']} | DEV_ID=0x{info['dev_id']:03X} | "
                        f"Flash={info['flash_kb']}KB | DP=0x{info['dp_id']:08X} | RDP={rdp_str}"
                    )
                    self.log("PROBE OK -> " + summary)
                    self.after(0, lambda: self.target_lbl.config(text="Trang thai Target: " + summary))
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"PROBE THAT BAI: {e}")
                self.after(0, lambda: self.target_lbl.config(text=f"Trang thai Target: Loi ({e})"))
        self._run_bg(work)

    def read_mem32(self, override_addr=None):
        def work():
            try:
                addr_str = override_addr or self.addr_var.get().strip()
                addr = int(addr_str, 0)
                clk = int(self.clk_var.get())
                d = self._selected_hid()
                dev = WinHidDevice(d["path"], d["in_len"], d["out_len"])
                try:
                    dap = CmsisDapClient(dev)
                    dap.connect_swd()
                    dap.swj_clock(clk)
                    dap.transfer_configure(2, 100, 0)
                    dap.swd_configure(0)
                    dap.swd_line_reset_to_swd()
                    dap.read_dp(0x00)
                    dap.write_abort(0x1E)
                    dap.write_dp(0x08, 0)
                    dap.write_dp(0x04, 0x50000000)
                    for _ in range(30):
                        if (dap.read_dp(0x04) & 0xA0000000) == 0xA0000000:
                            break
                        time.sleep(0.002)
                    dap.transfer([(0x01 | 0x00, 0x23000012)])
                    val = dap.mem_read32(addr)
                    dap.disconnect()
                    self.log(f"MEM[0x{addr:08X}] = 0x{val:08X}")
                finally:
                    dev.close()
            except Exception as e:
                self.log(f"LOI Read Mem32: {e}")
        self._run_bg(work)

    # -----------------------------------------------------------------------
    # OpenOCD Actions
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

    def send_serial_cmd(self, cmd: str):
        if not self.serial_port or not self.serial_port.is_open:
            self.log("Chua mo cong COM trong tab UART CLI.")
            return
        cmd = cmd.strip()
        if not cmd:
            return
        self.serial_port.write((cmd + "\r\n").encode("utf-8"))
        self.log(f"[UART TX] {cmd}")

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
