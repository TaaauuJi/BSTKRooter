#!/usr/bin/env python3
# BSTKRooter
# Copyright (c) 2026 Taaauu
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""XOR-encrypt res/su_c -> res/su_c.enc to defeat static AV pattern matching.

The same key must be used at runtime in RootTool.cpp to decrypt.
Key: 0xA7 (single-byte, simple but effective against signature scans).
"""

import pathlib, sys

KEY = 0xA7

def xor_bytes(data: bytes, key: int) -> bytes:
    return bytes(b ^ key for b in data)

def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    src = root / "res" / "su_c"
    dst = root / "res" / "su_c.enc"

    if not src.exists():
        print(f"[!] Source not found: {src}", file=sys.stderr)
        sys.exit(1)

    raw = src.read_bytes()
    enc = xor_bytes(raw, KEY)
    dst.write_bytes(enc)
    print(f"[+] Encrypted {len(raw)} bytes -> {dst}")
    print(f"    Key: 0x{KEY:02X}")

if __name__ == "__main__":
    main()
