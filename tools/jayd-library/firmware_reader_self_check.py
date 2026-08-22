#!/usr/bin/env python3
"""Build converter fixtures and exercise the firmware metadata reader on the host."""

from __future__ import annotations

import binascii
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CONVERTER = HERE / "jayd_library.py"
READER = ROOT / "src" / "Metadata" / "JaydMetadata.cpp"
CHECK = HERE / "firmware_reader_self_check.cpp"
HOST_INCLUDE = HERE / "host"
HEADER = struct.Struct("<8sHHHHIIIQQI16s")
SECTION = struct.Struct("<4sHHIIQ")


def repaired(data: bytearray) -> bytes:
    struct.pack_into("<I", data, 44, 0)
    struct.pack_into("<I", data, 44, binascii.crc32(data) & 0xFFFFFFFF)
    return bytes(data)


def section_index(data: bytes, kind: bytes) -> int:
    _, _, _, _, entry_size, count, *_ = HEADER.unpack_from(data)
    for index in range(count):
        if data[HEADER.size + index * entry_size:HEADER.size + index * entry_size + 4] == kind:
            return index
    raise AssertionError(f"missing section {kind!r}")


def fixtures(directory: Path) -> None:
    source = directory / "engine.json"
    source.write_text(json.dumps({
        "schema": "jayd-engine-interchange-1",
        "producer": "firmware-reader-self-check",
        "tracks": [{
            "source_id": "stable-source-1",
            "path": "safe/song.aac",
            "sample_rate": 44100,
            "duration_frames": 110250,
            "bpm": "128.125",
            "key": "8A",
            "rating": 4,
            "cues": [
                {"kind": "cue", "position_frames": 22050, "label": "Cue"},
                {"kind": "loop", "position_frames": 44100, "length_frames": 22050, "label": "Loop"},
            ],
            "beatgrid": [{
                "position_frames": 0,
                "bpm": "128.125",
                "beat_number": 1,
                "confidence": 9000,
            }],
            "phrases": [{
                "position_frames": 0,
                "kind": "intro",
                "confidence": 8000,
            }],
        }],
    }), encoding="utf-8")
    subprocess.run([
        sys.executable, str(CONVERTER), "engine-json", str(source),
        "-o", str(directory / "valid.jydm"),
    ], check=True, stdout=subprocess.DEVNULL)
    valid = (directory / "valid.jydm").read_bytes()

    corrupt = bytearray(valid)
    corrupt[-1] ^= 1
    (directory / "corrupt-crc.jydm").write_bytes(corrupt)
    (directory / "truncated.jydm").write_bytes(valid[:-1])

    oversized = bytearray(valid)
    struct.pack_into("<I", oversized, 20, 4097)
    (directory / "oversized-count.jydm").write_bytes(repaired(oversized))

    overflow = bytearray(valid)
    meta = section_index(overflow, b"META")
    struct.pack_into("<Q", overflow, HEADER.size + meta * SECTION.size + 16, 0xFFFFFFFFFFFFFFFC)
    (directory / "offset-overflow.jydm").write_bytes(repaired(overflow))

    overlap = bytearray(valid)
    cues = section_index(overlap, b"CUES")
    tracks = section_index(overlap, b"TRAK")
    track_offset = struct.unpack_from("<Q", overlap, HEADER.size + tracks * SECTION.size + 16)[0]
    struct.pack_into("<Q", overlap, HEADER.size + cues * SECTION.size + 16, track_offset)
    (directory / "overlapping-sections.jydm").write_bytes(repaired(overlap))

    unsupported = bytearray(valid)
    tracks = section_index(unsupported, b"TRAK")
    struct.pack_into("<H", unsupported, HEADER.size + tracks * SECTION.size + 4, 2)
    (directory / "unsupported-version.jydm").write_bytes(repaired(unsupported))

    traversal = bytearray(valid)
    old_path = b"safe/song.aac\0"
    new_path = b"../x/song.aac\0"
    assert len(old_path) == len(new_path)
    location = traversal.index(old_path)
    traversal[location:location + len(old_path)] = new_path
    (directory / "traversal-path.jydm").write_bytes(repaired(traversal))

    optional = bytearray(valid)
    _, version, endian, header_size, directory_entry_size, section_count, track_count, flags, file_size, directory_offset, crc, reserved = HEADER.unpack_from(optional)
    insert_at = directory_offset + section_count * directory_entry_size
    optional[insert_at:insert_at] = SECTION.pack(b"XOPT", 99, 1, 0, 0, insert_at + SECTION.size)
    for index in range(section_count):
        entry = directory_offset + index * directory_entry_size
        struct.pack_into("<Q", optional, entry + 16, struct.unpack_from("<Q", optional, entry + 16)[0] + SECTION.size)
    struct.pack_into("<I", optional, 16, section_count + 1)
    struct.pack_into("<Q", optional, 28, file_size + SECTION.size)
    (directory / "unknown-optional.jydm").write_bytes(repaired(optional))


def main() -> int:
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("c++ compiler is required")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        fixtures(directory)
        executable = directory / "firmware-reader-self-check"
        subprocess.run([
            compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
            "-I", str(HOST_INCLUDE), "-I", str(ROOT / "src" / "Metadata"),
            str(READER), str(CHECK), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), str(directory)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
