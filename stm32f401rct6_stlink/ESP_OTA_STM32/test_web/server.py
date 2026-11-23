"""Local test server: serves a minimal HTML page, hosts /firmware.bin over HTTP,
publishes test commands to MQTTS, and listens to esp32/status in real time.
"""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
import json
import os
import re
import socket
import ssl
import sys
import threading
import time
import uuid

import paho.mqtt.client as mqtt

MQTT_HOST = "m12dfadd.ala.asia-southeast1.emqxsl.com"
MQTT_PORT = 8883
MQTT_USER = "esp32"
MQTT_PASS = "1234"
MQTT_KEEPALIVE = 60
STATUS_TOPIC = "esp32/status"

ALLOWED_TOPICS = {
    "esp32/to_stm32/flash",
    "esp32/to_stm32/reset/stm32",
    "esp32/to_stm32/check",
    "esp32/to_stm32/reset/ec600n",
    "stm32/to_esp32/reset",
}

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_PATH = os.path.join(BASE_DIR, "index.html")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("PORT", "8080"))

_lock = threading.Lock()
_firmware_bytes = b""
_firmware_name = ""
_logs = []
_state = {
    "pc_mqtt_connected": False,
    "esp32_last_seen": "",
    "esp32_ip": "",
    "stm32_state": "UNKNOWN",   # CONNECTED | DISCONNECTED | UNKNOWN
    "stm32_detail": "Chua nhan duoc phan hoi tu ESP32",
    "flash_detail": "",
}

_mqtt_client = None
_mqtt_connected = threading.Event()


def add_log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    with _lock:
        _logs.append(line)
        if len(_logs) > 150:
            del _logs[: len(_logs) - 150]


def get_lan_ip() -> str:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except OSError:
        return "127.0.0.1"


def _handle_esp32_status(msg: str) -> None:
    add_log(f"[ESP32] {msg}")
    ts = time.strftime("%H:%M:%S")
    with _lock:
        _state["esp32_last_seen"] = ts

        m_ip = re.search(r"\bip=(\S+)", msg)
        if m_ip:
            _state["esp32_ip"] = m_ip.group(1)

        if "stm32=CONNECTED" in msg:
            _state["stm32_state"] = "CONNECTED"
            m_dp = re.search(r"stm32=CONNECTED\(([^)]+)\)", msg)
            _state["stm32_detail"] = f"DP={m_dp.group(1)}" if m_dp else "CONNECTED"
        elif "stm32=DISCONNECTED" in msg:
            _state["stm32_state"] = "DISCONNECTED"
            _state["stm32_detail"] = "Khong thay STM32 qua SWD"
        elif msg.startswith("stm32: CONNECTED"):
            _state["stm32_state"] = "CONNECTED"
            _state["stm32_detail"] = msg[len("stm32: "):]
        elif msg.startswith("stm32: DISCONNECTED"):
            _state["stm32_state"] = "DISCONNECTED"
            _state["stm32_detail"] = msg[len("stm32: "):]
        elif msg.startswith("flash:"):
            _state["flash_detail"] = msg
            if "FAILED download" in msg:
                add_log(
                    "Goi y: ESP32 khong tai duoc .bin tu PC! Kiem tra Windows Firewall "
                    f"co dang chan cong {PORT} hoac ESP32/PC co cung mang WiFi khong."
                )


def _start_mqtt_listener() -> None:
    global _mqtt_client

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            _mqtt_connected.set()
            with _lock:
                _state["pc_mqtt_connected"] = True
            client.subscribe(STATUS_TOPIC, qos=1)
            add_log(f"PC MQTTS da ket noi, dang lang nghe '{STATUS_TOPIC}'")
        else:
            with _lock:
                _state["pc_mqtt_connected"] = False
            add_log(f"PC MQTTS ket noi bi tu choi: {reason_code}")

    def on_disconnect(client, userdata, flags, reason_code, properties):
        _mqtt_connected.clear()
        with _lock:
            _state["pc_mqtt_connected"] = False
        add_log(f"PC MQTTS mat ket noi ({reason_code}), se tu ket noi lai...")

    def on_message(client, userdata, message):
        payload = message.payload.decode("utf-8", errors="replace").strip()
        if message.topic == STATUS_TOPIC:
            _handle_esp32_status(payload)
        else:
            add_log(f"MQTT <- {message.topic}: {payload}")

    client_id = f"pc_test_{uuid.uuid4().hex[:8]}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.tls_set_context(ssl.create_default_context())
    client.reconnect_delay_set(min_delay=2, max_delay=30)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    _mqtt_client = client
    try:
        client.connect_async(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
        client.loop_start()
    except Exception as exc:
        add_log(f"PC MQTTS loi khoi tao: {exc}")


def publish_mqtt(topic: str, payload: str) -> None:
    if _mqtt_client is not None and _mqtt_connected.is_set():
        info = _mqtt_client.publish(topic, payload=payload, qos=1)
        info.wait_for_publish(timeout=5.0)
        if not info.is_published():
            raise TimeoutError("Gui ban tin MQTT chua hoan tat")
        return

    connected = threading.Event()
    connect_rc = {"val": None}

    def on_connect(client, userdata, flags, reason_code, properties):
        connect_rc["val"] = reason_code
        connected.set()

    client_id = f"pc_pub_{uuid.uuid4().hex[:8]}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.tls_set_context(ssl.create_default_context())
    client.on_connect = on_connect

    client.connect(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
    client.loop_start()
    try:
        if not connected.wait(timeout=8.0):
            raise TimeoutError("Het thoi gian cho ket noi MQTTS")
        if connect_rc["val"] != 0:
            raise RuntimeError(f"MQTTS tu choi ket noi: {connect_rc['val']}")

        info = client.publish(topic, payload=payload, qos=1)
        info.wait_for_publish(timeout=5.0)
        if not info.is_published():
            raise TimeoutError("Gui ban tin MQTT chua hoan tat")
    finally:
        client.loop_stop()
        client.disconnect()


class TestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        return

    def _send_json(self, status: int, body: dict) -> None:
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        if parsed.path == "/":
            with open(INDEX_PATH, "rb") as f:
                html = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if parsed.path == "/info":
            lan_ip = get_lan_ip()
            with _lock:
                data = {
                    "lan_ip": lan_ip,
                    "port": PORT,
                    "bin_url": f"http://{lan_ip}:{PORT}/firmware.bin",
                    "filename": _firmware_name,
                    "size": len(_firmware_bytes),
                    "logs": list(_logs),
                    "status": dict(_state),
                }
            self._send_json(200, data)
            return

        if parsed.path == "/firmware.bin":
            with _lock:
                data = _firmware_bytes
                name = _firmware_name or "firmware.bin"
            if not data:
                self.send_error(404, "Chua co file .bin tren server")
                add_log(f"HTTP GET /firmware.bin tu {self.client_address[0]} -> 404 (chua co file)")
                return

            add_log(f"HTTP GET /firmware.bin tu {self.client_address[0]} -> 200 ({len(data)} bytes)")
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        self.send_error(404)

    def do_POST(self) -> None:
        global _firmware_bytes, _firmware_name
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))

        if parsed.path == "/upload":
            if length <= 0:
                self._send_json(400, {"ok": False, "error": "File rong"})
                return
            if length > 512 * 1024:
                self._send_json(413, {"ok": False, "error": "File qua lon (>512 KB)"})
                return

            body = self.rfile.read(length)
            qs = parse_qs(parsed.query)
            name = os.path.basename(qs.get("name", ["firmware.bin"])[0]) or "firmware.bin"
            with _lock:
                _firmware_bytes = body
                _firmware_name = name
            add_log(f"Da nap '{name}' ({len(body)} bytes) len local HTTP server")
            self._send_json(200, {"ok": True, "filename": name, "size": len(body)})
            return

        if parsed.path == "/mqtt":
            raw = self.rfile.read(length)
            try:
                req = json.loads(raw.decode("utf-8"))
                topic = req.get("topic", "").strip()
                payload = str(req.get("payload", ""))
                if topic not in ALLOWED_TOPICS:
                    self._send_json(400, {"ok": False, "error": f"Topic khong hop le: {topic}"})
                    return
                if topic == "esp32/to_stm32/flash":
                    with _lock:
                        has_bin = len(_firmware_bytes) > 0
                    if not has_bin and f":{PORT}/firmware.bin" in payload:
                        add_log("Canh bao: chua upload file .bin len local server")
                publish_mqtt(topic, payload)
                add_log(f"MQTT -> {topic} | payload: {payload}")
                self._send_json(200, {"ok": True})
            except Exception as exc:
                add_log(f"MQTT loi: {exc}")
                self._send_json(500, {"ok": False, "error": str(exc)})
            return

        self.send_error(404)


def main() -> None:
    lan_ip = get_lan_ip()
    add_log(f"Local test web: http://127.0.0.1:{PORT}/")
    add_log(f"URL cho ESP32 tai .bin: http://{lan_ip}:{PORT}/firmware.bin")
    _start_mqtt_listener()
    server = ThreadingHTTPServer(("0.0.0.0", PORT), TestHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nDa dung server.")
        if _mqtt_client is not None:
            _mqtt_client.loop_stop()
            _mqtt_client.disconnect()
        server.server_close()


if __name__ == "__main__":
    main()
