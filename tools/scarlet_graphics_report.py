#!/usr/bin/env python3
"""Read-only inspection of a Scarlet graphics crash; no rebuild or game run.

Collects guest instructions around the crashed frame and recent NVN callers,
plus the exact x86-64 fault from the matching local game DLL's PE image.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import io
import mmap
from pathlib import Path
import re
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_ARCH_X86, CS_MODE_ARM, CS_MODE_64
from analyze import eh_frame_functions
from lifter import Lifter, Unsupported
from nso import Nso

MODULES = (('main', 0x7000000000), ('sdk', 0x7006000000), ('subsdk0', 0x7008000000))


def read_log(path):
    raw = path.read_bytes()
    return raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8-sig', 'replace')


def crash_details(log):
    fault = re.search(r'host pc ([0-9a-fA-F]+) \(lasgame\.dll\+([0-9a-fA-F]+)\)', log)
    frames = re.findall(r'#\d+\s+([0-9a-fA-F]+)\s+f_([0-9a-fA-F]+)(?:_resume)?\+([0-9a-fA-F]+)', log)
    calls = re.findall(r'^\s+(nvn\w+)\s+x0=([0-9a-fA-F]+)\s+x1=([0-9a-fA-F]+)\s+lr=([0-9a-fA-F]+)', log, re.M)
    return fault, frames, calls


def pe_layout(image):
    if image[:2] != b'MZ':
        raise ValueError('not a PE file')
    pe = struct.unpack_from('<I', image, 0x3C)[0]
    if image[pe:pe + 4] != b'PE\0\0':
        raise ValueError('invalid PE signature')
    machine, count = struct.unpack_from('<HH', image, pe + 4)
    optional_size = struct.unpack_from('<H', image, pe + 20)[0]
    opt = pe + 24
    if machine != 0x8664 or struct.unpack_from('<H', image, opt)[0] != 0x20B or optional_size < 144:
        raise ValueError('expected an x86-64 PE32+ game DLL')
    sections = []
    for i in range(count):
        at = opt + optional_size + 40 * i
        virtual_size, rva, raw_size, raw_off = struct.unpack_from('<IIII', image, at + 8)
        sections.append((rva, virtual_size, raw_off, raw_size))
    directory_count = struct.unpack_from('<I', image, opt + 108)[0]
    exception = struct.unpack_from('<II', image, opt + 112 + 3 * 8) if directory_count > 3 else (0, 0)
    return sections, exception


def pe_offset(sections, rva, size=1):
    for start, virtual_size, raw_off, raw_size in sections:
        delta = rva - start
        if 0 <= delta and delta + size <= raw_size:
            return raw_off + delta
    raise ValueError(f'RVA 0x{rva:x}+0x{size:x} has no file-backed PE section')


def pe_function(image, sections, exception, pc):
    rva, size = exception
    if not rva or not size:
        return None
    offset = pe_offset(sections, rva, size)
    if offset + size > len(image):
        raise ValueError('truncated PE exception directory')
    lo, hi = 0, size // 12
    while lo < hi:
        mid = (lo + hi) // 2
        if struct.unpack_from('<I', image, offset + 12 * mid)[0] <= pc:
            lo = mid + 1
        else:
            hi = mid
    if lo:
        start, end, unwind = struct.unpack_from('<III', image, offset + 12 * (lo - 1))
        if start <= pc < end:
            return start, end
    return None


def native_context(out, dll, fault, frames):
    if not fault:
        out.write('\n[native] no lasgame.dll fault RVA found in this log\n')
        return
    if not dll.is_file():
        out.write(f'\n[native] missing DLL: {dll}\n')
        return
    host_pc, rva = (int(x, 16) for x in fault.groups())
    out.write(f'\n[native] DLL={dll} size={dll.stat().st_size} fault_rva=0x{rva:x}\n')
    with dll.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        out.write(f'[native sha256] {hashlib.sha256(image).hexdigest()}\n')
        sections, exception = pe_layout(image)
        bounds = pe_function(image, sections, exception, rva)
        # If .pdata is absent, a frame naming the exact fault PC is a bounded
        # fallback. Never guess an x86 instruction boundary by starting at PC-N.
        if bounds is None:
            for host, guest, delta in frames:
                if int(host, 16) == host_pc and int(delta, 16) <= rva:
                    bounds = rva - int(delta, 16), rva + 0x60
                    break
        if bounds is None or not bounds[0] <= rva < bounds[1] or rva - bounds[0] > 0x20000:
            out.write('[native] no bounded function start; skipping unsynchronized disassembly\n')
            return
        start, end = bounds
        end = min(end, rva + 0x60)
        raw_off = pe_offset(sections, start, end - start)
        if raw_off + end - start > len(image):
            raise ValueError('truncated native function')
        out.write(f'[native function] RVA=0x{start:x}..0x{end:x}; instructions use DLL RVAs\n')
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        decoded = list(md.disasm(bytes(image[raw_off:raw_off + end - start]), start))
        hit = next((i for i, ins in enumerate(decoded) if ins.address == rva), None)
        if hit is None:
            out.write('[native] fault is not an instruction boundary in the current DLL; it may differ from the crashed build\n')
        else:
            for ins in decoded[max(0, hit - 16):hit + 13]:
                out.write(f'{"=>" if ins.address == rva else "  "} RVA 0x{ins.address:x}: '
                          f'{ins.bytes.hex():24s} {ins.mnemonic:8s} {ins.op_str}\n')


def guest_context(out, name, base, n, addresses):
    lo, length = n.text
    hi = lo + length
    roots = sorted({lo, hi} | {p for p in eh_frame_functions(n) if lo <= p < hi})
    ranges = []
    for address in sorted(addresses):
        off = address - base
        if not lo <= off < hi:
            continue
        index = bisect.bisect_right(roots, off) - 1
        start = roots[index]
        if off - start > 0x3000:
            start = max(lo, off - 0x80)
        end = min(roots[index + 1], max(off + 0x180, start + 0x3000), hi)
        # Bound oversized functions while always retaining the requested PC.
        end = min(end, max(start + 0x3000, off + 4))
        ranges.append((start, end))
    merged = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    lifter = Lifter(base, 'c')
    for start, end in merged:
        out.write(f'\n[guest region] {name} 0x{base+start:x}..0x{base+end:x}\n')
        for off in range(start, end - 3, 4):
            word = struct.unpack_from('<I', n.image, off)[0]
            pc = base + off
            ins = next(md.disasm(bytes(n.image[off:off + 4]), pc, count=1), None)
            asm = f'{ins.mnemonic:8s} {ins.op_str}' if ins else '(undecoded)'
            out.write(f'{"=>" if pc in addresses else "  "} 0x{pc:x}: {word:08x} {asm}\n')
            try:
                lifted = lifter.lift(word, pc)
                if lifted.kind == 'op' and lifted.code:
                    out.write(f'       C: {lifted.code}\n')
            except Unsupported as exc:
                out.write(f'       lifter: {exc}\n')


def make_report(exefs, boot_log, dll):
    log = read_log(boot_log)
    fault, frames, calls = crash_details(log)
    out = io.StringIO()
    out.write('Scarlet graphics crash inspection\n'
              'Read-only: no game execution, regeneration, patches or build.\n'
              'Use the DLL from this crash; rebuilding before collection can invalidate native addresses.\n'
              'Recent NVN calls are context, not proof that the last call caused the corruption.\n')
    out.write(f'[log] {boot_log}\n')
    for line in log.splitlines():
        if any(key in line for key in ('[dispatch]', '[gfx-milestone]', '*** exception', 'host pc', 'guest:')):
            out.write(line + '\n')
        elif re.match(r'\s+x\d+\s+[0-9a-fA-F]{16}', line):
            out.write(line + '\n')
    targets = {int(frames[0][1], 16)} if frames else set()
    for name, x0, x1, lr in calls[-16:]:
        out.write(f'[recent NVN] {name} x0={x0} x1={x1} lr={lr}\n')
        if int(lr, 16) >= 4:
            targets.add(int(lr, 16) - 4)
    missing = []
    for name, base in MODULES:
        path = exefs / name
        if not path.is_file():
            missing.append(name)
            out.write(f'[missing module] {name}\n')
            continue
        n = Nso(str(path))
        out.write(f'[module] {name} build={n.build_id} base=0x{base:x}\n')
        guest_context(out, name, base, n, targets)
    try:
        native_context(out, dll, fault, frames)
    except (ValueError, struct.error, OSError) as exc:
        out.write(f'\n[native inspection error] {exc}\n')
    return out.getvalue(), missing


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('exefs', type=Path)
    ap.add_argument('--log', type=Path, default=Path('scarlet_boot.log'))
    ap.add_argument('--dll', type=Path, default=Path('build/lasgame.dll'))
    ap.add_argument('--output', type=Path, default=Path('scarlet_graphics_report.txt'))
    args = ap.parse_args()
    output = args.output.resolve()
    inputs = {args.log.resolve(), args.dll.resolve()}
    inputs.update((args.exefs / name).resolve() for name, base in MODULES)
    aliases_input = output.exists() and any(p.exists() and output.samefile(p) for p in inputs)
    if output in inputs or aliases_input or output.is_relative_to(args.exefs.resolve()) or output.suffix.lower() != '.txt':
        ap.error('output must be a diagnostic .txt outside ExeFS and distinct from the input log/DLL')
    try:
        report, missing = make_report(args.exefs, args.log, args.dll)
        args.output.write_text(report, encoding='utf-8')
    except (OSError, ValueError, struct.error, AssertionError) as exc:
        ap.error(str(exc))
    print(f'Report: {args.output} ({len(report)} characters); missing modules: {", ".join(missing) or "none"}')
    return 2 if missing else 0


if __name__ == '__main__':
    raise SystemExit(main())
