#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The BLE name / serial of a Kode Dot from its Bluetooth MAC, and back.

    python3 tools/serial.py 44:bd:8d:78:8b:aa      # -> KODE24QP6QH2XAQ  KODE-24QP-6QH2-XAQ
    python3 tools/serial.py KODE24QP6QH2XAQ        # -> 44:bd:8d:78:8b:aa (check symbol verified)
    python3 tools/serial.py --selftest

Same rule as main/serial.c and kode-sdk khal_identity.c: "KODE", the 48-bit
MAC as ten Crockford Base32 symbols (left-padded to 50 bits), one Luhn mod 32
check symbol.
"""
import sys

ALPHA = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
VECTORS = [  # kode-sdk's own vectors, plus one read by the official app
    ("e8:f6:0a:e7:6c:1d", "KODE78YR5EEV0XS"),
    ("e8:f6:0a:e7:6c:1e", "KODE78YR5EEV0YQ"),
    ("00:00:00:00:00:01", "KODE0000000001Y"),
    ("ff:ff:ff:ff:ff:ff", "KODE7ZZZZZZZZZ2"),
    ("44:bd:8d:78:8b:aa", "KODE24QP6QH2XAQ"),
]


def luhn32(sym):
    s, f = 0, 2
    for x in reversed(sym):
        p = f * x
        s += p // 32 + p % 32
        f = 1 if f == 2 else 2
    return (32 - s % 32) % 32


def serial(mac: str) -> str:
    v = int(mac.replace(":", "").replace("-", ""), 16)
    sym = [(v >> (50 - (i + 1) * 5)) & 31 for i in range(10)]
    return "KODE" + "".join(ALPHA[s] for s in sym) + ALPHA[luhn32(sym)]


def pretty(s: str) -> str:
    return f"KODE-{s[4:8]}-{s[8:12]}-{s[12:15]}"


def mac(s: str) -> str:
    s = s.upper().replace("-", "")
    if not s.startswith("KODE") or len(s) != 15:
        raise ValueError("a serial is KODE + 11 symbols")
    sym = [ALPHA.index(c) for c in s[4:14]]
    if ALPHA[luhn32(sym)] != s[14]:
        raise ValueError("check symbol does not match")
    v = 0
    for x in sym:
        v = (v << 5) | x
    return ":".join(f"{b:02x}" for b in v.to_bytes(7, "big")[1:])


def selftest() -> int:
    bad = 0
    for m, want in VECTORS:
        got = serial(m)
        back = mac(want)
        if got != want or back != m:
            bad += 1
            print(f"FAIL {m}: {got} / {back}")
    print(f"{len(VECTORS) - bad}/{len(VECTORS)} pass")
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    a = sys.argv[1]
    if a == "--selftest":
        sys.exit(selftest())
    if a.upper().startswith("KODE"):
        print(mac(a))
    else:
        s = serial(a)
        print(s, pretty(s))
