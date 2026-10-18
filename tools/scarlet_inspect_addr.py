#!/usr/bin/env python3
"""Inspect one guest ARM64 address inside Scarlet's extracted NSOs.

Prints the module/offset, nearest dynamic symbols, relocations that resolve to
this address, references from rodata/data, and a short Capstone disassembly.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from nso import (
    Nso,
    R_AARCH64_ABS64,
    R_AARCH64_GLOB_DAT,
    R_AARCH64_JUMP_SLOT,
    R_AARCH64_RELATIVE,
)

MODULES = (
    ("main", 0x7000000000),
    ("sdk", 0x7006000000),
    ("subsdk0", 0x7008000000),
)


def resolved_reloc_target(n: Nso, base: int, rel):
    off, typ, symidx, add = rel
    if typ == R_AARCH64_RELATIVE:
        return base + add
    if typ in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
        if 0 <= symidx < len(n.syms):
            name, info, shndx, value, size = n.syms[symidx]
            if shndx:
                return base + value + add
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exefs")
    ap.add_argument("address", help="absolute guest address, e.g. 0x70065ad8c0")
    args = ap.parse_args()

    exefs = Path(args.exefs)
    addr = int(args.address, 0)

    found = None
    for name, base in MODULES:
        path = exefs / name
        if not path.is_file():
            continue
        n = Nso(str(path))
        if base <= addr < base + n.size:
            found = (name, base, n)
            break

    if not found:
        print(f"[addr] 0x{addr:x}: outside loaded Scarlet modules")
        return 2

    name, base, n = found
    off = addr - base
    print(f"[addr] 0x{addr:x} -> {name}+0x{off:x} build={n.build_id}")
    print(f"[seg] text=0x{n.text[0]:x}+0x{n.text[1]:x} ro=0x{n.rodata[0]:x}+0x{n.rodata[1]:x} data=0x{n.data[0]:x}+0x{n.data[1]:x}")

    syms = []
    for idx, (sname, info, shndx, value, size) in enumerate(n.syms):
        if not sname or not shndx:
            continue
        if value <= off:
            syms.append((off - value, idx, sname, value, size, info))
    syms.sort(key=lambda x: x[0])
    print("[symbols]")
    if syms:
        for d, idx, sname, value, size, info in syms[:8]:
            contains = size and value <= off < value + size
            print(f"  {'*' if contains else ' '} #{idx} {sname} +0x{d:x} value=0x{value:x} size=0x{size:x} type={info & 0xf}")
    else:
        print("  (none before address)")

    print("[relocs -> target]")
    hits = 0
    for rel in n.relocs:
        target = resolved_reloc_target(n, base, rel)
        if target == addr:
            roff, typ, symidx, add = rel
            sname = n.syms[symidx][0] if 0 <= symidx < len(n.syms) else ""
            print(f"  reloc@+0x{roff:x} type={typ} sym={symidx}:{sname} add={add:+#x}")
            hits += 1
    if not hits:
        print("  (none)")

    print("[qword refs]")
    refs = 0
    targets = {addr, off}
    for segname, (soff, ssize) in (("rodata", n.rodata), ("data", n.data)):
        end = soff + ssize
        for p in range(soff, end - 7, 8):
            v = struct.unpack_from("<Q", n.image, p)[0]
            if v in targets:
                print(f"  {segname}+0x{p-soff:x} module+0x{p:x}: 0x{v:x}")
                refs += 1
                if refs >= 32:
                    break
        if refs >= 32:
            break
    if not refs:
        print("  (none)")

    text_off, text_size = n.text
    if not (text_off <= off < text_off + text_size):
        print("[disasm] address is outside .text")
        return 0

    lo = max(text_off, (off - 0x40) & ~3)
    hi = min(text_off + text_size, off + 0x120)
    code = bytes(n.image[lo:hi])
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    print("[disasm]")
    for ins in md.disasm(code, base + lo):
        mark = "=>" if ins.address == addr else "  "
        print(f"{mark} 0x{ins.address:011x}: {ins.mnemonic:8s} {ins.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
