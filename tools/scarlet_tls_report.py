#!/usr/bin/env python3
"""Read-only NSO inspection for a TPIDR_EL0 value reset during Scarlet boot.

Run with the project's Python venv; no build or game execution is needed.
Only diagnostic text is written. ExeFS files are never modified.
"""
from __future__ import annotations

import argparse
import bisect
import io
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

from analyze import eh_frame_functions
from nso import Nso

MODULES = (('main', 0x7000000000), ('sdk', 0x7006000000), ('subsdk0', 0x7008000000))
SDK_BUILD = '3644a6760469451cabc99f00c540acc2a64aee41'
START_NAMES = {
    '_ZN2nn4init5StartEmmPFvvES2_', '_ZN2nn4init5StartEmmPFvvES2_S2_',
    '_init_libc0', '_init_libc1', '_init_libc2', '__nnDetailInitLibc0',
    '__nnDetailInitLibc1', '__nnDetailInitLibc2', '__nnmusl_init_dso',
}


def tls_writes(n):
    """All exact MSR TPIDR_EL0,Xn encodings, including XZR (explicit clear)."""
    lo, size = n.text
    text = memoryview(n.image)[lo:lo + (size & ~3)]
    return [(lo + 4 * i, word & 31)
            for i, (word,) in enumerate(struct.iter_unpack('<I', text))
            if word & 0xFFFFFFE0 == 0xD51BD040]


def direct_callers(n, targets):
    """Module-relative B/BL callers of the candidate writer function starts."""
    found = {target: [] for target in targets}
    lo, size = n.text
    for i, (word,) in enumerate(struct.iter_unpack('<I', memoryview(n.image)[lo:lo + (size & ~3)])):
        if word & 0x7C000000 != 0x14000000:
            continue
        imm = word & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
        pc = lo + 4 * i
        target = pc + 4 * imm
        if target in found:
            found[target].append(pc)
    return found


def report_module(out, name, base, n):
    def say(line=''):
        out.write(line + '\n')

    lo, size = n.text
    hi = lo + size
    funcs = sorted((value, sname, length) for sname, info, shndx, value, length in n.syms
                   if shndx and info & 15 == 2 and lo <= value < hi)
    values = [f[0] for f in funcs]
    try:
        unwind = sorted(a for a in eh_frame_functions(n) if lo <= a < hi)
    except (ValueError, struct.error) as exc:
        unwind = []
        say(f'[unwind warning] {exc}')

    def describe(off):
        idx = bisect.bisect_right(values, off) - 1
        if idx < 0:
            return '(no preceding function symbol)'
        value, sname, length = funcs[idx]
        suffix = '' if length and off < value + length else ' [nearest symbol; outside its reported size]'
        return f'{sname}+0x{off-value:x}{suffix}'

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def context(off, before=0x60, after=0x40):
        if not lo <= off < hi:
            say('  (outside .text)')
            return
        start, end = max(lo, (off-before) & ~3), min(hi, off + after)
        for pc in range(start, end - 3, 4):
            code = bytes(n.image[pc:pc + 4])
            ins = next(md.disasm(code, base + pc, count=1), None)
            text = f'{ins.mnemonic:8s} {ins.op_str}' if ins else f'.word 0x{int.from_bytes(code, "little"):08x}'
            say(f'  {"=>" if pc == off else "  "} 0x{base+pc:x}: {text}')

    say(f'\n[module] {name} base=0x{base:x} build={n.build_id} text=0x{lo:x}+0x{size:x}')
    writes = tls_writes(n)
    say(f'[TPIDR_EL0 writes] {len(writes)}')
    owners = set()
    for off, reg in writes:
        source = 'xzr (explicit zero)' if reg == 31 else f'x{reg}'
        say(f'\n[MSR] 0x{base+off:x} source={source} nearest={describe(off)}')
        context(off)
        owners.add(off)  # a tiny setter/callback may enter at the MSR itself
        for candidates in (values, unwind):
            idx = bisect.bisect_right(candidates, off) - 1
            if idx >= 0:
                owners.add(candidates[idx])

    callers = direct_callers(n, owners) if owners else {}
    for target, sites in sorted(callers.items()):
        say(f'\n[writer-start callers] 0x{base+target:x} {describe(target)}: {len(sites)} direct sites')
        for pc in sites[:8]:
            say(f'  caller 0x{base+pc:x}: {describe(pc)}')
            context(pc, before=0x20, after=0x10)
        if len(sites) > 8:
            say(f'  ({len(sites)-8} further direct sites omitted)')

    for sname, info, shndx, value, length in n.syms:
        if shndx and sname in START_NAMES:
            say(f'\n[startup symbol] {sname} @0x{base+value:x} size=0x{length:x}')
            context(value, before=0, after=min(max(length, 0x100), 0x300))

    # Values observed in the failing log, inspected only against that SDK ID.
    # In particular, x0 at <rtld_mark> is not assumed to be a callback contract.
    if name == 'sdk' and n.build_id == SDK_BUILD:
        for off, label, length in (
            (0x30, 'SDK DT_INIT', 0x100),
            (0x2DBF90, 'nn::init::Start from boot log', 0x140),
            (0x37A690, 'x0 observed at the no-op rtld_mark callback', 0x180),
            (0x5051B8, 'SDK constructor-path frame from boot log', 0x180),
        ):
            say(f'\n[logged SDK focus] {label} @0x{base+off:x}: {describe(off)}')
            context(off, before=0x20, after=length)
    elif name == 'sdk':
        say('\n[notice] SDK Build ID differs from the supplied crash; fixed-address focuses were skipped.')
    return len(writes)


def make_report(exefs):
    out = io.StringIO()
    out.write('Scarlet writable TLS reset inspection\n'
              'Observed: TPIDR_EL0 starts at 0x7090100000, then is zero at malloc.\n'
              'Static instruction inspection only: a listed MSR is a candidate, not proof it executed.\n'
              'Nearest symbols may not contain the address. No game files are modified or executed.\n')
    missing, count = [], 0
    for name, base in MODULES:
        path = exefs / name
        if not path.is_file():
            missing.append(name)
            out.write(f'\n[missing module] {name}\n')
            continue
        count += report_module(out, name, base, Nso(str(path)))
    out.write(f'\n[summary] MSR sites={count}; missing modules={", ".join(missing) or "none"}\n')
    return out.getvalue(), missing


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('exefs', type=Path)
    ap.add_argument('--output', type=Path, default=Path('scarlet_tls_report.txt'))
    args = ap.parse_args()
    if not args.exefs.is_dir():
        ap.error(f'ExeFS directory not found: {args.exefs}')
    if args.exefs.resolve() in (args.output.resolve(), *args.output.resolve().parents):
        ap.error('The report output must be outside the ExeFS directory.')
    report, missing = make_report(args.exefs)
    args.output.write_text(report, encoding='utf-8')
    print(f'[tls-report] Saved: {args.output.resolve()} ({len(report)} characters)')
    return 2 if missing else 0


if __name__ == '__main__':
    raise SystemExit(main())
