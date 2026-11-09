#!/usr/bin/env python3
"""从 merged-binary.bin 提取纯 app image（供 OTA 使用）。

ESP-IDF 的 `idf.py merge-bin` 产出从 flash 0x0 开始的整包，
而 OTA（esp_ota_write）只接受从 app 分区起始的纯 app 镜像。
本脚本按分区表定位 app 分区，裁掉尾部 0xFF 填充，并校验镜像头与 SHA-256。

用法：
    python3 scripts/extract_app_image.py \
        --merged build/merged-binary.bin \
        --output build/xiaozhi.bin
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_LENGTH = 0x4000
PARTITION_ENTRY_SIZE = 0x20
PARTITION_MAGIC = b"\xAA\x50"
TYPE_APP = 0x00
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 0x0009
# esp_image_header_t 中 hash_appended 的字节偏移
HASH_APPENDED_OFFSET = 23


def find_app_partition(data: bytes) -> tuple[int, int, str]:
    """在分区表中找到第一个 app 分区，返回 (offset, size, label)。"""
    end = PARTITION_TABLE_OFFSET + PARTITION_TABLE_LENGTH
    for i in range(PARTITION_TABLE_OFFSET, end, PARTITION_ENTRY_SIZE):
        if data[i:i + 2] != PARTITION_MAGIC:
            continue
        if data[i + 2] != TYPE_APP:
            continue
        offset = struct.unpack_from("<I", data, i + 4)[0]
        size = struct.unpack_from("<I", data, i + 8)[0]
        label = data[i + 12:i + 28].split(b"\x00")[0].decode("utf-8", "ignore")
        return offset, size, label
    raise SystemExit("分区表中未找到 app 分区")


def trim_trailing_ff(blob: bytes) -> bytes:
    end = len(blob)
    while end > 0 and blob[end - 1] == 0xFF:
        end -= 1
    return blob[:end]


def validate_app_image(image: bytes) -> None:
    if len(image) < 32:
        raise SystemExit(f"app image 太小: {len(image)} bytes")
    if image[0] != ESP_IMAGE_MAGIC:
        raise SystemExit(
            f"不是 app image：magic=0x{image[0]:02X}，应为 0x{ESP_IMAGE_MAGIC:02X}"
        )
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32S3_CHIP_ID:
        raise SystemExit(f"chip_id=0x{chip_id:04X}，不是 ESP32-S3")
    if image[HASH_APPENDED_OFFSET] != 1:
        raise SystemExit("镜像未附加 SHA-256（hash_appended=0）")
    expected = image[-32:]
    actual = hashlib.sha256(image[:-32]).digest()
    if expected != actual:
        raise SystemExit(
            f"SHA-256 自校验失败: stored={expected.hex()} calc={actual.hex()}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--merged", default="build/merged-binary.bin")
    parser.add_argument("--output", default="build/xiaozhi.bin")
    args = parser.parse_args()

    merged_path = Path(args.merged)
    if not merged_path.exists():
        raise SystemExit(f"找不到 {merged_path}")

    data = merged_path.read_bytes()
    offset, size, label = find_app_partition(data)
    app_region = data[offset:offset + size]
    image = trim_trailing_ff(app_region)
    validate_app_image(image)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)

    print(
        f"app 分区: {label} @ 0x{offset:X} (容量 0x{size:X})\n"
        f"已写出 {output_path}: {len(image)} bytes\n"
        f"sha256={image[-32:].hex()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
