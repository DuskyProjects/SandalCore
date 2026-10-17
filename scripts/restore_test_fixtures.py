#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
"""Restore legacy test fixtures from the checked-in, verbatim retail capture.

The source is src/protocol/bootstrap/bootstrap_data.h, extracted from the
original replay_data.bin. No server encoder is used to produce expected bytes.
Packet 22's first logical bunch consists of its two captured partial payloads:
3545 bits starting at 213, followed by 1314 bits starting at 3807 (4859 total).
"""
import argparse
import hashlib
from pathlib import Path
import re
import sys


PACKET_HASHES = {
    22: "5e0ebd5e506126cdd7336fc238047619a1f70b8de9a52f381c7bb84110aadd90",
    78: "4b706c18e02d6d91342de5c9d14d21b6e9f15a139906432f96d1a588e11f6102",
    79: "3fb75fd9b2ddba44a6dd7c2916cda8a675fc029d29b36feeb61a39d43e0073ea",
    104: "f65a768f31072988f66fdd670f64b3a2265515a230f23bf2a30655c80cb82080",
}
PC_HASH = "78a53043cfdfa7118b3a69f2b4c84cba8cce211e2e8fac6c7edc8bd48fae980c"


def read_bits(data: bytes, start: int, count: int) -> int:
    if start < 0 or count < 0 or start + count > len(data) * 8:
        raise ValueError("captured bit range exceeds its source packet")
    return sum(((data[(start + i) // 8] >> ((start + i) % 8)) & 1) << i for i in range(count))


def captured_packet(source: str, index: int) -> bytes:
    match = re.search(r"kPacket" + str(index) + r"_raw\[\]\s*=\s*\{(.*?)\};", source, re.S)
    if not match:
        raise ValueError(f"checked-in capture has no raw packet {index}")
    data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    if hashlib.sha256(data).hexdigest() != PACKET_HASHES[index]:
        raise ValueError(f"captured packet {index} changed; review fixture layout before restoring")
    return data


def fixtures(source: str) -> dict[str, bytes]:
    packets = {index: captured_packet(source, index) for index in PACKET_HASHES}
    for index, name_length_bit in ((79, 1353), (104, 1624)):
        data = packets[index]
        name = bytes(read_bits(data, name_length_bit + 32 + i * 8, 8) for i in range(11))
        if read_bits(data, 176, 13) != 1636 or read_bits(data, name_length_bit, 32) != 11 or name != b"RandomChar\0":
            raise ValueError(f"captured packet {index} no longer matches the legacy name site")

    pc = bytearray((4859 + 7) // 8)
    destination = 0
    for start, count in ((213, 3545), (3807, 1314)):
        for bit in range(start, start + count):
            pc[destination // 8] |= read_bits(packets[22], bit, 1) << (destination % 8)
            destination += 1
    if hashlib.sha256(pc).hexdigest() != PC_HASH:
        raise ValueError("reassembled PC capture failed its fixture checksum")
    return {
        "captured_pkt_78.bin": packets[78],
        "captured_pkt_79.bin": packets[79],
        "captured_pkt_104.bin": packets[104],
        "captured_pc_spawn_reassembled.bin": bytes(pc),
    }


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=root / "src/protocol/tools")
    args = parser.parse_args()
    try:
        restored = fixtures((root / "src/protocol/bootstrap/bootstrap_data.h").read_text(encoding="utf-8"))
        # Check existing artifacts before writing any. Preserve local fixtures.
        for name, data in restored.items():
            path = args.output_dir / name
            if path.exists() and path.read_bytes() != data:
                raise ValueError(f"{path} differs from the checked-in capture; keeping the existing file")
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for name, data in restored.items():
            path = args.output_dir / name
            if not path.exists():
                path.write_bytes(data)
                print(f"Restored {path.name}: {len(data)} captured bytes")
        return 0
    except (OSError, ValueError) as error:
        print(f"Cannot restore legacy test fixtures: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
