#!/usr/bin/env python3
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
