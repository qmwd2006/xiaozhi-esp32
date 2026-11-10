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
- 不能设 firmware.force=1：会无视版本比较，导致无限 OTA 循环。
- 版本号固定为固件实际版本，设备升级后版本相等即停止，天然自终止。

用法：
    python3 ota_server.py [--port 8066] [--bin path/to/xiaozhi.bin]
"""

from __future__ import annotations

import argparse
import json
import logging
import os
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

        # 解析设备上报的当前固件版本（决定是否需要升级）
        reported_version = ""
        reported_board = ""
        if raw:
            try:
                info = json.loads(raw.decode("utf-8", "ignore"))
                app = info.get("application") or {}
                reported_version = str(app.get("version") or "")
                board = info.get("board") or {}
                reported_board = str(board.get("type") or "")
            except Exception as exc:
                logger.debug("解析设备上报信息失败: %s", exc)
        if reported_version:
            logger.info("设备当前固件版本: %s%s", reported_version,
                        f" (board={reported_board})" if reported_board else "")

        if BIN_PATH is None or not BIN_PATH.exists():
            logger.error("固件文件不存在: %s", BIN_PATH)
            self._send_json({"error": "firmware not available"}, 503)
            return

        payload = {
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone_offset": TIMEZONE_OFFSET_MIN,
            },
            "firmware": {
                "version": FIRMWARE_VERSION,
                "url": f"{PUBLIC_BASE_URL}/xiaozhi.bin",
            },
            # 必须携带，否则设备回退到 MQTT
            "websocket": {
                "url": WEBSOCKET_URL,
                "token": WEBSOCKET_TOKEN,
            },
        }
        logger.info("返回固件 %s -> %s", FIRMWARE_VERSION, payload["firmware"]["url"])
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
    global BIN_PATH, FIRMWARE_VERSION, PUBLIC_BASE_URL

    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8066)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--bin", default="xiaozhi.bin")
    parser.add_argument("--version", default="2.5.3")
    parser.add_argument("--public-host", default="192.168.31.236",
                        help="设备访问本机使用的主机地址")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s - %(message)s",
    )

    BIN_PATH = Path(args.bin).resolve()
    FIRMWARE_VERSION = args.version
    PUBLIC_BASE_URL = f"http://{args.public_host}:{args.port}"

    if not BIN_PATH.exists():
        logger.error("固件文件不存在: %s", BIN_PATH)
        return 1

    size = BIN_PATH.stat().st_size
    with BIN_PATH.open("rb") as f:
        head = f.read(32)
    if not head or head[0] != 0xE9:
        logger.error("不是合法 app image（magic 应为 0xE9）: %s", BIN_PATH)
        return 1

    logger.info("固件: %s (%d bytes)", BIN_PATH, size)
    logger.info("版本: %s", FIRMWARE_VERSION)
    logger.info("OTA 地址: %s/xiaozhi/ota/", PUBLIC_BASE_URL)
    logger.info("固件下载: %s/xiaozhi.bin", PUBLIC_BASE_URL)

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
