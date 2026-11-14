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

- 支持按板型分发：`--bin <board_type>=<path>` 可重复指定，设备上报的
  board.type 命中则下发该板固件。未命中时若配置了 `--default-bin` 则用之，
  否则返回 404（避免把 A 板固件刷到 B 板）。单板用法 `--bin path` 仍然兼容。

用法：
    python3 ota_server.py [--port 8066] [--bin path/to/xiaozhi.bin]
    python3 ota_server.py --bin esp32-s3-audio-board=a.bin \
                          --bin cyberai-speaker=b.bin
"""

from __future__ import annotations

import argparse
import json
import logging
import struct
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

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

# 按板型分发的固件表：board_type -> app image 路径。
# 单板模式下用 DEFAULT_BOARD_KEY 作为唯一键，兼容旧的 --bin path 用法。
BOARD_BINS: dict[str, Path] = {}
DEFAULT_BOARD_KEY = ""

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


def _resolve_bin(
    board_type: str,
    bins: dict[str, Path] | None = None,
    default_key: str | None = None,
) -> Path | None:
    """按设备上报的 board.type 选择固件；返回 None 表示没有匹配项。"""
    registry = BOARD_BINS if bins is None else bins
    default = DEFAULT_BOARD_KEY if default_key is None else default_key
    key = (board_type or "").strip().lower()
    if key and key in registry:
        return registry[key]
    # 单板模式：只有一个固件时，无论设备报什么板型都用它（兼容旧行为）
    if default and default in registry:
        return registry[default]
    return None


class OtaHandler(BaseHTTPRequestHandler):
    server_version = "xiaozhi-ota/1.0"

    def log_message(self, fmt: str, *args) -> None:
        logger.info("%s - %s", self.address_string(), fmt % args)

    @property
    def _registry(self) -> dict[str, Path]:
        return getattr(self.server, "board_bins", BOARD_BINS)

    @property
    def _default_key(self) -> str:
        return getattr(self.server, "default_board_key", DEFAULT_BOARD_KEY)

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
            bins = {
                key: {
                    "path": str(p),
                    "size": p.stat().st_size if p.exists() else 0,
                    "version": _read_app_desc(p).get("version", "") if p.exists() else "",
                }
                for key, p in self._registry.items()
            }
            self._send_json(
                {
                    "ok": True,
                    "firmware_version": FIRMWARE_VERSION,
                    "default_board": self._default_key or None,
                    "bins": bins,
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

        bin_path = _resolve_bin(reported_board, self._registry, self._default_key)
        if bin_path is None:
            logger.warning("设备板型 %r 没有对应固件（已配置: %s），拒绝下发",
                           reported_board, ", ".join(sorted(self._registry)) or "无")
            self._send_json({"error": "no firmware for board"}, 404)
            return
        if not bin_path.exists():
            logger.error("固件文件不存在: %s", bin_path)
            self._send_json({"error": "firmware not available"}, 503)
            return

        bin_desc = _read_app_desc(bin_path)
        version = bin_desc.get("version") or FIRMWARE_VERSION
        firmware = {
            "version": version,
            "url": f"{PUBLIC_BASE_URL}/xiaozhi.bin?board={reported_board}",
        }

        # 同版本号重新构建时，用 bin 内嵌的 app 描述判断设备是否已经是这份固件。
        same_build = bool(
            reported_sha
            and bin_desc.get("elf_sha256")
            and reported_sha.lower() == bin_desc["elf_sha256"].lower()
        )
        force = False
        if not same_build and _firmware_is_newer_or_equal(
                reported_version, version):
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
        logger.info("返回固件 %s%s -> %s (board=%s)", version,
                    " (force)" if force else "", firmware["url"],
                    reported_board or self._default_key or "?")
        self._send_json(payload)

    def _serve_bin(self) -> None:
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        board = ""
        for part in query.split("&"):
            if part.startswith("board="):
                board = part[len("board="):]
                break
        board = unquote(board)
        bin_path = _resolve_bin(board, self._registry, self._default_key)
        if bin_path is None or not bin_path.exists():
            self._send_text("firmware not available", 404)
            return
        data = bin_path.read_bytes()
        logger.info("下发固件: %s (board=%s, %d bytes)",
                    bin_path.name, board or self._default_key or "?", len(data))
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
    global BOARD_BINS, DEFAULT_BOARD_KEY

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8066)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--bin", action="append", default=None,
                        help="固件 app image。可写成 <board_type>=<path> 按板型分发，"
                             "可重复；写成 <path> 时作为全局默认固件")
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

    PUBLIC_BASE_URL = f"http://{args.public_host}:{args.port}"
    ALWAYS_FORCE = args.force
    WEBSOCKET_URL = args.websocket_url
    WEBSOCKET_TOKEN = args.websocket_token
    MQTT_CONTROL_ENDPOINT = args.mqtt_control_endpoint
    MQTT_CONTROL_USERNAME = args.mqtt_control_username
    MQTT_CONTROL_PASSWORD = args.mqtt_control_password
    MQTT_CONTROL_KEEPALIVE = args.mqtt_control_keepalive

    # 解析固件表：<board>=<path> 或 <path>（默认固件）
    for entry in args.bin or ["xiaozhi.bin"]:
        board_type, sep, path_text = entry.partition("=")
        if sep and board_type and path_text:
            key = board_type.strip().lower()
            path = Path(path_text).resolve()
        else:
            key = ""
            path = Path(entry).resolve()

        if not path.exists():
            logger.error("固件文件不存在: %s", path)
            return 1
        with path.open("rb") as f:
            head = f.read(32)
        if not head or head[0] != 0xE9:
            logger.error("不是合法 app image（magic 应为 0xE9）: %s", path)
            return 1

        desc = _read_app_desc(path)
        if key:
            BOARD_BINS[key] = path
            logger.info("板型 %-24s -> %s (%d bytes, %s)", key, path,
                        path.stat().st_size, desc.get("version", "?"))
        else:
            DEFAULT_BOARD_KEY = key or "default"
            BOARD_BINS.setdefault(DEFAULT_BOARD_KEY, path)
            logger.info("默认固件 -> %s (%d bytes, %s)", path,
                        path.stat().st_size, desc.get("version", "?"))

    # 兼容旧接口：BIN_PATH 指向默认固件（如有）
    BIN_PATH = BOARD_BINS.get(DEFAULT_BOARD_KEY) if DEFAULT_BOARD_KEY else None
    if BIN_PATH is not None:
        desc = _read_app_desc(BIN_PATH)
        FIRMWARE_VERSION = args.version or desc.get("version") or "0.0.0"
        logger.info("构建: %s %s, elf_sha256=%s", desc.get("date", ""), desc.get("time", ""),
                    desc.get("elf_sha256", "")[:16])
    if len(BOARD_BINS) == 1:
        FIRMWARE_VERSION = args.version or _read_app_desc(
            next(iter(BOARD_BINS.values()))).get("version") or FIRMWARE_VERSION

    logger.info("同版本重构建升级: %s", "总是" if ALWAYS_FORCE else "按 sha256 差异自动判定")
    logger.info("OTA 地址: %s/xiaozhi/ota/", PUBLIC_BASE_URL)
    logger.info("固件下载: %s/xiaozhi.bin?board=<board_type>", PUBLIC_BASE_URL)
    logger.info("下发 websocket: %s", WEBSOCKET_URL)
    logger.info("旁路 MQTT 控制: %s", MQTT_CONTROL_ENDPOINT or "未启用")

    server = ThreadingHTTPServer((args.host, args.port), OtaHandler)
    server.board_bins = BOARD_BINS
    server.default_board_key = DEFAULT_BOARD_KEY
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logger.info("收到中断，退出")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
