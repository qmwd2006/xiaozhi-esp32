#!/usr/bin/env python3
"""本地 OTA 服务：给小智设备下发自编译固件。

端点：
    POST /xiaozhi/ota/   版本检查（设备上报系统信息，返回固件信息）
    GET  /xiaozhi.bin    OTA app image（纯 app 镜像，非 merged-binary）
    GET  /health         自检

设计要点（均有固件源码依据）：
- 必须返回 websocket 段：否则 InitializeProtocol() 会 fallback 到 MQTT，
  设备连不上（main/application.cc）。
- 不能返回 activation 段：会进入激活流程，打断升级。
- 设备固件只按版本号比较（main/ota.cc IsNewVersionAvailable），同版本号的
  重新构建无法触发升级。因此这里用 app 描述里的 elf_sha256 / compile_time
  比对：设备上报的哈希与本地 bin 不一致时才返回 firmware.force=1，
  设备升级后哈希一致即停止，天然自终止，不会无限 OTA。
- 只在服务端版本不低于设备版本时才允许 force，避免降级。

用法：
    python3 ota_server.py [--port 8066] [--bin path/to/xiaozhi.bin]
"""

from __future__ import annotations

import argparse
import json
import logging
import struct
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

logger = logging.getLogger("ota_server")

# 与官方 OTA 响应保持一致（实测 api.tenclass.net 返回的就是这组值）
WEBSOCKET_URL = "wss://api.tenclass.net/xiaozhi/v1/"
WEBSOCKET_TOKEN = "test-token"
TIMEZONE_OFFSET_MIN = 480  # 东八区

# 由 main() 填充
BIN_PATH: Path | None = None
FIRMWARE_VERSION = "2.5.3"
PUBLIC_BASE_URL = ""
ALWAYS_FORCE = False

# 旁路 MQTT 控制通道（可选）。仅当配置了 endpoint 时下发 mqtt_control 段，
# 官方/纯 ws 场景保持 None，设备不会启动旁路客户端。
MQTT_CONTROL_ENDPOINT: str | None = None
MQTT_CONTROL_USERNAME = ""
MQTT_CONTROL_PASSWORD = ""
MQTT_CONTROL_KEEPALIVE = 240

# app image 布局：image header + segment header 之后是 esp_app_desc_t。
# esp_app_desc_t: magic(4) secure_version(4) reserv1(8) version(32) project(32)
#                 time(16) date(16) idf_ver(32) app_elf_sha256(32) reserv2(...)
_APP_DESC_OFFSET = 0x20
_APP_DESC_SIZE = 256


def _read_app_desc(path: Path) -> dict[str, str]:
    """从纯 app image 中解析 esp_app_desc_t 的关键字段。"""
    try:
        with path.open("rb") as f:
            data = f.read(_APP_DESC_OFFSET + _APP_DESC_SIZE)
    except OSError as exc:
        logger.warning("读取固件失败: %s", exc)
        return {}
    if len(data) < _APP_DESC_OFFSET + 0x90 + 32:
        return {}

    def text(offset: int, size: int) -> str:
        raw = data[_APP_DESC_OFFSET + offset:_APP_DESC_OFFSET + offset + size]
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace")

    magic = struct.unpack_from("<I", data, _APP_DESC_OFFSET)[0]
    if magic != 0xABCD5432:
        logger.warning("固件 app 描述 magic 不匹配: 0x%08x", magic)
        return {}

    return {
        "version": text(0x10, 32),
        "project_name": text(0x30, 32),
        "time": text(0x50, 16),
        "date": text(0x60, 16),
        "idf_ver": text(0x70, 32),
        "elf_sha256": data[_APP_DESC_OFFSET + 0x90:_APP_DESC_OFFSET + 0x90 + 32].hex(),
    }


def _parse_version(version: str) -> tuple[int, ...] | None:
    try:
        return tuple(int(part) for part in version.split("."))
    except (ValueError, AttributeError):
        return None


def _firmware_is_newer_or_equal(device: str, server: str) -> bool:
    """服务端版本不低于设备版本时才允许 force，避免降级。"""
    device_parts = _parse_version(device)
    server_parts = _parse_version(server)
    if device_parts is None or server_parts is None:
        return True
    return server_parts >= device_parts


class OtaHandler(BaseHTTPRequestHandler):
    server_version = "xiaozhi-ota/1.0"

    def log_message(self, fmt: str, *args) -> None:
        logger.info("%s - %s", self.address_string(), fmt % args)

    # ---------- 响应工具 ----------

    def _send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_text(self, text: str, status: int = 200) -> None:
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---------- 路由 ----------

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/health":
            self._send_json(
                {
                    "ok": True,
                    "firmware_version": FIRMWARE_VERSION,
                    "bin": str(BIN_PATH) if BIN_PATH else None,
                    "bin_size": BIN_PATH.stat().st_size if BIN_PATH and BIN_PATH.exists() else 0,
                }
            )
            return
        if path == "/xiaozhi.bin":
            self._serve_bin()
            return
        self._send_text("not found", 404)

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0]
        # 读取并丢弃请求体（设备会上报系统信息 JSON）
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length > 0 else b""
        if path.rstrip("/") == "/xiaozhi/ota":
            self._handle_ota(raw)
            return
        self._send_text("not found", 404)

    # ---------- 业务 ----------

    def _handle_ota(self, raw: bytes) -> None:
        device = self.headers.get("Device-Id", "?")
        client = self.headers.get("Client-Id", "?")
        logger.info("OTA 请求: Device-Id=%s Client-Id=%s body=%d bytes",
                    device, client, len(raw))

        # 解析设备上报的当前固件信息（决定是否需要升级）
        reported_version = ""
        reported_board = ""
        reported_sha = ""
        reported_compile_time = ""
        if raw:
            try:
                info = json.loads(raw.decode("utf-8", "ignore"))
                app = info.get("application") or {}
                reported_version = str(app.get("version") or "")
                reported_sha = str(app.get("elf_sha256") or "")
                reported_compile_time = str(app.get("compile_time") or "")
                board = info.get("board") or {}
                reported_board = str(board.get("type") or "")
            except Exception as exc:
                logger.debug("解析设备上报信息失败: %s", exc)
        if reported_version:
            logger.info("设备当前固件: %s%s%s", reported_version,
                        f" (board={reported_board})" if reported_board else "",
                        f" sha256={reported_sha[:12]}…" if reported_sha else "")

        if BIN_PATH is None or not BIN_PATH.exists():
            logger.error("固件文件不存在: %s", BIN_PATH)
            self._send_json({"error": "firmware not available"}, 503)
            return

        firmware = {
            "version": FIRMWARE_VERSION,
            "url": f"{PUBLIC_BASE_URL}/xiaozhi.bin",
        }

        # 同版本号重新构建时，用 bin 内嵌的 app 描述判断设备是否已经是这份固件。
        bin_desc = _read_app_desc(BIN_PATH)
        same_build = bool(
            reported_sha
            and bin_desc.get("elf_sha256")
            and reported_sha.lower() == bin_desc["elf_sha256"].lower()
        )
        force = False
        if not same_build and _firmware_is_newer_or_equal(
                reported_version, FIRMWARE_VERSION):
            if ALWAYS_FORCE or (reported_sha and bin_desc.get("elf_sha256")):
                force = True
                firmware["force"] = 1
                logger.info(
                    "设备固件与本地 bin 不同 (device=%s bin=%s build=%s %s)，强制升级",
                    reported_sha[:12] or reported_compile_time or "?",
                    bin_desc.get("elf_sha256", "")[:12] or "?",
                    bin_desc.get("date", ""), bin_desc.get("time", ""))

        payload = {
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone_offset": TIMEZONE_OFFSET_MIN,
            },
            "firmware": firmware,
            # 必须携带，否则设备回退到 MQTT
            "websocket": {
                "url": WEBSOCKET_URL,
                "token": WEBSOCKET_TOKEN,
            },
        }
        # 可选：旁路 MQTT 控制通道。per-device topic，用 MAC 的十六进制串。
        if MQTT_CONTROL_ENDPOINT:
            mac = (device or "").replace(":", "").replace("-", "").lower() or "unknown"
            payload["mqtt_control"] = {
                "endpoint": MQTT_CONTROL_ENDPOINT,
                "client_id": f"xiaozhi-ctl-{mac}",
                "username": MQTT_CONTROL_USERNAME,
                "password": MQTT_CONTROL_PASSWORD,
                "keepalive": MQTT_CONTROL_KEEPALIVE,
                "subscribe_topic": f"xiaozhi/{mac}/down",
                "publish_topic": f"xiaozhi/{mac}/up",
            }
            logger.info("下发 mqtt_control: %s (mac=%s)", MQTT_CONTROL_ENDPOINT, mac)
        logger.info("返回固件 %s%s -> %s", FIRMWARE_VERSION,
                    " (force)" if force else "", firmware["url"])
        self._send_json(payload)

    def _serve_bin(self) -> None:
        if BIN_PATH is None or not BIN_PATH.exists():
            self._send_text("firmware not available", 503)
            return
        data = BIN_PATH.read_bytes()
        logger.info("下发固件: %s (%d bytes)", BIN_PATH.name, len(data))
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    global BIN_PATH, FIRMWARE_VERSION, PUBLIC_BASE_URL, ALWAYS_FORCE
    global WEBSOCKET_URL, WEBSOCKET_TOKEN
    global MQTT_CONTROL_ENDPOINT, MQTT_CONTROL_USERNAME, MQTT_CONTROL_PASSWORD
    global MQTT_CONTROL_KEEPALIVE

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8066)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--bin", default="xiaozhi.bin")
    parser.add_argument("--version", default=None,
                        help="覆盖固件版本号；默认从 bin 内嵌的 app 描述读取")
    parser.add_argument("--force", action="store_true",
                        help="忽略 sha256 比对，始终返回 firmware.force=1")
    parser.add_argument("--public-host", default="192.168.31.236",
                        help="设备访问本机使用的主机地址")
    parser.add_argument("--websocket-url", default=WEBSOCKET_URL,
                        help="下发给设备的 websocket 地址；指向自建网关可用 "
                             "ws://192.168.31.178:8989/xiaozhi/v1/")
    parser.add_argument("--websocket-token", default=WEBSOCKET_TOKEN,
                        help="下发给设备的 websocket token")
    parser.add_argument("--mqtt-control-endpoint", default=None,
                        help="旁路 MQTT 控制通道 broker 地址 host:port；"
                             "配置后 OTA 响应携带 mqtt_control 段（默认不下发）")
    parser.add_argument("--mqtt-control-username", default="")
    parser.add_argument("--mqtt-control-password", default="")
    parser.add_argument("--mqtt-control-keepalive", type=int, default=240)
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s - %(message)s",
    )

    BIN_PATH = Path(args.bin).resolve()
    PUBLIC_BASE_URL = f"http://{args.public_host}:{args.port}"
    ALWAYS_FORCE = args.force
    WEBSOCKET_URL = args.websocket_url
    WEBSOCKET_TOKEN = args.websocket_token
    MQTT_CONTROL_ENDPOINT = args.mqtt_control_endpoint
    MQTT_CONTROL_USERNAME = args.mqtt_control_username
    MQTT_CONTROL_PASSWORD = args.mqtt_control_password
    MQTT_CONTROL_KEEPALIVE = args.mqtt_control_keepalive

    if not BIN_PATH.exists():
        logger.error("固件文件不存在: %s", BIN_PATH)
        return 1

    size = BIN_PATH.stat().st_size
    with BIN_PATH.open("rb") as f:
        head = f.read(32)
    if not head or head[0] != 0xE9:
        logger.error("不是合法 app image（magic 应为 0xE9）: %s", BIN_PATH)
        return 1

    desc = _read_app_desc(BIN_PATH)
    FIRMWARE_VERSION = args.version or desc.get("version") or "0.0.0"

    logger.info("固件: %s (%d bytes)", BIN_PATH, size)
    logger.info("版本: %s", FIRMWARE_VERSION)
    if desc:
        logger.info("构建: %s %s, elf_sha256=%s", desc.get("date", ""), desc.get("time", ""),
                    desc.get("elf_sha256", "")[:16])
    logger.info("同版本重构建升级: %s", "总是" if ALWAYS_FORCE else "按 sha256 差异自动判定")
    logger.info("OTA 地址: %s/xiaozhi/ota/", PUBLIC_BASE_URL)
    logger.info("固件下载: %s/xiaozhi.bin", PUBLIC_BASE_URL)
    logger.info("下发 websocket: %s", WEBSOCKET_URL)
    logger.info("旁路 MQTT 控制: %s", MQTT_CONTROL_ENDPOINT or "未启用")

    server = ThreadingHTTPServer((args.host, args.port), OtaHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("收到中断，退出")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
