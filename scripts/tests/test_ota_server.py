"""Unit tests for scripts/ota_server.py board-aware firmware dispatch."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ota_server


def _make_app_image(version: str, project: str = "xiaozhi") -> bytes:
    """Build the minimal bytes ota_server needs: header magic + esp_app_desc_t."""
    header = bytearray(ota_server._APP_DESC_OFFSET)
    header[0] = 0xE9

    desc = bytearray(ota_server._APP_DESC_SIZE)
    struct.pack_into("<I", desc, 0x00, 0xABCD5432)  # magic_word
    desc[0x10:0x10 + 32] = version.encode().ljust(32, b"\0")
    desc[0x30:0x30 + 32] = project.encode().ljust(32, b"\0")
    desc[0x50:0x50 + 16] = b"12:00:00".ljust(16, b"\0")
    desc[0x60:0x60 + 16] = b"Jan  1 2026".ljust(16, b"\0")
    desc[0x70:0x70 + 32] = b"v6.1".ljust(32, b"\0")
    desc[0x90:0x90 + 32] = bytes(range(32))  # deterministic elf_sha256
    return bytes(header) + bytes(desc)


class ResolveBinTests(unittest.TestCase):
    def test_exact_board_match(self):
        a, b = Path("/x/a.bin"), Path("/x/b.bin")
        bins = {"esp32-s3-audio-board": a, "cyberai-speaker": b}
        self.assertEqual(ota_server._resolve_bin("cyberai-speaker", bins, ""), b)
        self.assertEqual(ota_server._resolve_bin("ESP32-S3-Audio-Board", bins, ""), a)

    def test_unknown_board_without_default_is_none(self):
        bins = {"cyberai-speaker": Path("/x/b.bin")}
        self.assertIsNone(ota_server._resolve_bin("other", bins, ""))
        self.assertIsNone(ota_server._resolve_bin("", bins, ""))

    def test_single_bin_mode_falls_back_for_any_board(self):
        only = Path("/x/only.bin")
        bins = {"default": only}
        self.assertEqual(ota_server._resolve_bin("anything", bins, "default"), only)
        self.assertEqual(ota_server._resolve_bin("", bins, "default"), only)


class AppDescTests(unittest.TestCase):
    def test_parse_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "app.bin"
            path.write_bytes(_make_app_image("9.9.9", "cyberai"))
            desc = ota_server._read_app_desc(path)
            self.assertEqual(desc["version"], "9.9.9")
            self.assertEqual(desc["project_name"], "cyberai")
            self.assertEqual(len(desc["elf_sha256"]), 64)

    def test_rejects_bad_magic(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.bin"
            path.write_bytes(b"\x00" * (ota_server._APP_DESC_OFFSET + ota_server._APP_DESC_SIZE))
            self.assertEqual(ota_server._read_app_desc(path), {})


if __name__ == "__main__":
    unittest.main()
