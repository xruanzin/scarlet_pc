"""Generate C for a whole NSO module.

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import bisect
import os
import platform
import re
import struct
import sys
import time
from collections import Counter

import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, hx, sext
from nso import Nso, R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000
# Verified by Scarlet's protected-call/longjmp boot trace. Addresses are NSO
# relative and deliberately scoped to this exact main Build ID.
SCARLET_SETJMP_THUNKS = {
    'ca44e7dd62e61a2a4e8889b3319ae173375770fd': {0x3017B70},
}

# Scarlet 1.0.0 PLT thunks for tiny nn::os helpers that dominate slow-scene
# function-entry profiles. The SDK definitions are also replaced through
# --hle/hle_symbols.txt; mapping the main thunks avoids the extra PLT/GOT +
# indirect dispatcher trip for calls originating in main.
SCARLET_MAIN_DIRECT_HLE = {
    'ca44e7dd62e61a2a': {
        0x30177A0: '_ZN2nn2os16GetCurrentThreadEv',
        0x3018960: '_ZN2nn2os11GetTlsValueENS0_7TlsSlotE',
        0x3019B40: '_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE',

        # Verified hot leaf functions from the Scarlet 1.0.0 slow-scene capture.
        # These are pure getters/constants and are substantially cheaper as one
        # host call than as lifted fragments + dispatcher traffic.
        0x27036E0: 'scarlet_hot_vec_d0_size',
        0x270386C: 'scarlet_hot_slot_c4',
        0x26B7418: 'scarlet_hot_false',
    },
}

# Tail import thunks whose resolved target is a defined SDK function. Calling
# that lifted SDK function directly keeps its exact implementation but avoids
# the main PLT/GOT thunk and an indirect dispatcher lookup on every call.
SCARLET_MAIN_DIRECT_GUEST = {
    'ca44e7dd62e61a2a': {
        # Keep this table for lifted SDK helpers that need exact guest bodies.
        # SignalLightEvent moved to SCARLET_MAIN_DIRECT_HLE: its uncontended
        # state transition is host-fast and its waiter path falls back to SDK.
    },
}


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


MODULE_BASES = {
    'main': 0x7000000000,
    'sdk': 0x7006000000,
    'subsdk0': 0x7008000000,
}


def _sx(v, bits_):
    sign = 1 << (bits_ - 1)
    return (v ^ sign) - sign


def detect_plt_imports(n, base):
    """Return {absolute PLT entry: imported symbol name} for standard AArch64 stubs."""
    rel = {}
    for off, typ, symidx, add in n.relocs:
        if typ in (R_AARCH64_JUMP_SLOT, R_AARCH64_GLOB_DAT) and 0 <= symidx < len(n.syms):
            rel[off] = symidx

    out = {}
    lo, size = n.text
    hi = lo + size
    for off in range(lo, hi - 12, 4):
        w0 = n.u32(off)
        w1 = n.u32(off + 4)
        w2 = n.u32(off + 8)
        w3 = n.u32(off + 12)

        # adrp x16, page ; ldr x17,[x16,#imm] ; add x16,x16,#imm ; br x17
        if (w0 & 0x9F00001F) != 0x90000010:
            continue
        if (w1 & 0xFFC003FF) != 0xF9400211:
            continue
        if (w2 & 0xFFC003FF) != 0x91000210:
            continue
        if w3 != 0xD61F0220:
            continue

        immlo = (w0 >> 29) & 3
        immhi = (w0 >> 5) & 0x7FFFF
        pages = _sx((immhi << 2) | immlo, 21)
        pc = base + off
        page = (pc & ~0xFFFull) + (pages << 12)
        got = page + (((w1 >> 10) & 0xFFF) << 3)
        got_off = got - base
        symidx = rel.get(got_off)
        if symidx is None:
            continue
        name = n.syms[symidx][0]
        if name:
            out[base + off] = name
    return out


def sibling_export_targets(path, current_name):
    """Resolve unique exports in the sibling main/sdk/subsdk0 modules."""
    root = os.path.dirname(os.path.abspath(path))
    found = {}
    for mod, base in MODULE_BASES.items():
        if mod == current_name:
            continue
        p = os.path.join(root, mod)
        if not os.path.exists(p):
            continue
        try:
            other = Nso(p)
        except Exception:
            continue
        for name, info, shndx, value, size in other.syms:
            if not name or not shndx:
                continue
            found.setdefault(name, set()).add(base + value)
    return {name: next(iter(v)) for name, v in found.items() if len(v) == 1}


class Module:
    def __init__(self, name, path, base, simd='asm', trace_stores=False):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base, simd, trace_stores)
        self.cache = {}
        self.stats = Counter()
        self.mu = None
        self.setjmp_targets = {base + a for a in SCARLET_SETJMP_THUNKS.get(n.build_id, set())} if name == 'main' else set()
        self.setjmp_sites = {}
        self.direct_guest = {}
        # Guest entry -> host HLE symbol. Direct BL sites use this table too,
        # bypassing the generated f_<addr> wrapper when the target is known.
        self.direct_hle = {}
        self.always_inline_bytes = 96

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */')
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)

        # Dynamic symbols are useful function boundaries, but newer Nintendo
        # SDKs also publish code addresses as STT_NOTYPE/data-like symbols.
        # If a defined symbol value itself lands in .text, treat it as a valid
        # callable entry regardless of ELF symbol type.
        for name, info, shndx, value, size in n.syms:
            if shndx and self.in_text(b + value):
                starts.add(b + value)

        # Function pointers, callbacks and vtables are frequently materialized
        # by relocations rather than direct BL/ADR references. RELATIVE gives
        # B+A; ABS64/GLOB_DAT/JUMP_SLOT give S+A. Discover every relocation
        # whose resolved value is inside this module's .text so indirect BLR/BR
        # can dispatch to it after relocation.
        reloc_target = {}
        for off, typ, symidx, add in n.relocs:
            target = None
            if typ == R_AARCH64_RELATIVE:
                target = b + add
            elif typ in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT):
                if 0 <= symidx < len(n.syms):
                    name, info, shndx, value, size = n.syms[symidx]
                    if shndx:
                        target = b + value + add
            if target is not None:
                reloc_target[off] = target
                if self.in_text(target):
                    starts.add(target)

        # rtld / __nnmusl_init_dso call DT_INIT and constructor-array entries
        # indirectly. These pointers are not guaranteed to appear as BL/ADR
        # targets, so explicitly seed them as callable entries.
        for off in (getattr(n, 'init', 0), getattr(n, 'fini', 0)):
            if off and self.in_text(b + off):
                starts.add(b + off)

        for arr, size in ((getattr(n, 'init_array', 0), getattr(n, 'init_array_size', 0)),
                          (getattr(n, 'fini_array', 0), getattr(n, 'fini_array_size', 0))):
            if not arr or not size:
                continue
            for slot in range(arr, arr + size, 8):
                target = reloc_target.get(slot)
                if target is None and slot + 8 <= len(n.image):
                    raw = n.u64(slot)
                    if self.in_text(raw):
                        target = raw
                    elif self.in_text(b + raw):
                        target = b + raw
                if target is not None and self.in_text(target):
                    starts.add(target)

        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
            self.mu = mu
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        r = self._jump_table_fallthrough(br_pc, entry, end)
        if r is None:
            r = self._jump_table_taken(br_pc, entry, end)
        return r

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at):
        """Run start..br for each index value (set at address set_at), collect targets."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)

        def step_to(frm, to):
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        targets = []
        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if set_at != start and not step_to(start, set_at):
                    return None
                self._set_index(mu, [idx], i)
                if not step_to(set_at, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cw = self.word(g - 4) if g - 4 >= entry else 0
            if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
                continue
            limit = bits(cw, 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            return self._emulate_table(br_pc, entry, end, s, bits(cw, 5, 5), count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        a = guard - 4
        while a >= entry and guard - a <= 4 * 8:
            w = self.word(a)
            # SUBS (imm) with Rd = zr, i.e. CMP #imm
            if (w & 0x7F80001F) == 0x7100001F:
                cmp = a
                break
            if self.lift(a).kind != 'op':
                return None
            a -= 4
        if cmp is None:
            return None
        w = self.word(cmp)
        if bits(w, 22, 1):
            return None
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        idx = bits(w, 5, 5)
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        idx_regs = self._index_regs(start, cmp, idx)
        mu = self.emu()
        targets = []

        def step_to(frm, to):
            # single-step: Unicorn's `until` is unreliable mid translation-block
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table
                # bases computed before loops or branches are known
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if not step_to(start, cmp):
                    return None
                self._set_index(mu, idx_regs, i)
                if not step_to(cmp, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if (w & 31) == rn:
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(jt)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    # A loaded target can be a local code pointer too. Try the
                    # local labels before global dispatch, even when the table
                    # recognizer cannot prove where the pointer came from.
                    tables[a] = 'all'
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def indirect_resume_entries(self, entries, hle):
        """Fragments needing entry at an arbitrary PC after an indirect jump.

        ADR/ADRP targets and external branches split unwind functions into
        smaller emitted fragments. A switch target may be inside a *different*
        fragment from its BR. Cover the enclosing unwind interval, including
        fragments that do not themselves contain a BR. These are conservative
        text intervals between unwind starts, not inferred symbol sizes.
        """
        roots = sorted({a + self.base for a in eh_frame_functions(self.nso)
                        if self.in_text(a + self.base)} | {self.lo, self.hi})
        if len(roots) == 2:  # stripped/no unwind table: use the known entries
            roots = sorted(set(self.boundaries()) | {self.lo, self.hi})
        groups = {bisect.bisect_right(roots, a) - 1
                  for a, (end, labels, tables) in entries.items() if tables}
        groups.update(bisect.bisect_right(roots, pc) - 1 for pc in self.setjmp_sites)
        return {a for a, (end, labels, tables) in entries.items()
                if a not in hle and end > a + 4
                and bisect.bisect_right(roots, a) - 1 in groups}

    def prepare_setjmp(self, entries):
        self.setjmp_sites = {}
        if not self.setjmp_targets:
            return
        order = sorted(entries)
        for entry, (end, labels, tables) in entries.items():
            for pc in range(entry, end, 4):
                ins = self.lift(pc)
                if ins.kind == 'bl' and ins.target in self.setjmp_targets:
                    ret = pc + 4
                    owner = order[bisect.bisect_right(order, ret) - 1]
                    if not owner <= ret < entries[owner][0]:
                        raise ValueError(f'setjmp continuation outside text: {ret:x}')
                    self.setjmp_sites[pc] = (ins.target, owner)

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            h = self.direct_hle.get(t)
            if h is not None:
                return [f'c->x[30] = {hx(a + 4)}; {sanitize(h)}(c);']
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'{{ static RtCallCache ic; c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch_cached(c, &ic); }}']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle, resumable=False):
        # Tiny lifted functions are ubiquitous in Scarlet (getters, wrappers,
        # STL helpers). ThinLTO can see across chunks, but its size heuristic
        # still leaves a large amount of host call/Cpu-state traffic behind.
        # Force-inline compact ordinary entries while retaining their external
        # out-of-line copy for the dispatch table / indirect calls.
        force_inline = (not resumable and end > entry and
                        end - entry <= self.always_inline_bytes)
        attr = '__attribute__((always_inline, hot)) ' if force_inline else ''
        out = [f'{attr}void {fname(entry)}(Cpu * restrict c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out

        # A direct-guest replacement is only an entry fast path. If this
        # fragment belongs to an indirect-resume interval, keep the original
        # lifted body as _resume so dispatch_lookup can still enter at an
        # arbitrary interior PC. This also keeps main_table.c's resume-range
        # declaration valid instead of leaving an undefined *_resume symbol.
        direct_target = self.direct_guest.get(entry)
        if direct_target is not None:
            if not resumable:
                self._refs = {direct_target}
                out.append(f'    {fname(direct_target)}(c);')
                out.append('}')
                return out
            out.insert(0, f'void {fname(entry)}_resume(Cpu *c);')
            out += [f'    {fname(direct_target)}(c);',
                    '}', f'void {fname(entry)}_resume(Cpu *c) {{',
                    '    goto L_dispatch;']
        else:
            out.append(f'    FN_ENTER({hx(entry)});')
            if resumable:
                # Normal direct calls always start at entry. Only the runtime's
                # range lookup calls _resume with an interior c->pc. Keep identical
                # signatures so fragment transitions remain tail calls.
                out.insert(0, f'void {fname(entry)}_resume(Cpu *c);')
                out += [f'    c->pc = {hx(entry)};',
                        f'    __attribute__((musttail)) return {fname(entry)}_resume(c);',
                        '}', f'void {fname(entry)}_resume(Cpu * restrict c) {{',
                        '    goto L_dispatch;']
        batch = []
        refs = set()
        if direct_target is not None:
            refs.add(direct_target)
        jump_helpers = []

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        # Functions with jump tables also get the all-instruction fallback: a table whose emulation
        # missed some cases then still reaches the right instruction instead of aborting.
        all_labels = bool(tables) or resumable
        if all_labels:
            labels = set(range(entry, end, 4))
        rando_here = [a for a in range(entry, end, 4) if a - self.base in RANDO_CODE]
        if rando_here:
            labels = set(labels)
            for a in rando_here:
                for w in RANDO_CODE[a - self.base]:
                    v = self.lifter.lift(w, a)
                    if v.kind in ('b', 'bcond') and isinstance(v.target, int) and entry <= v.target < end:
                        labels.add(v.target)
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a - self.base in RANDO_CODE:  # randomizer patch site: the active seed picks the instruction
                flush()
                out.append(f'    {{ extern uint32_t rando_word(uint64_t); uint32_t rw = rando_word({hx(a)});')
                first = True
                for w in RANDO_CODE[a - self.base]:
                    v = self.lifter.lift(w, a)
                    out.append(f'    {"if" if first else "else if"} (rw == {hx(w)}) {{ ' + ' '.join(self.render_single(v, a, jump, refs)) + ' }')
                    first = False
                out.append('    else { ' + ' '.join(self.render_single(ins, a, jump, refs)) + ' } }')
                last = 'op'
                continue
            if a in PATCHES:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + PATCHES[a](self, a))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if a in self.setjmp_sites:
                    target, owner = self.setjmp_sites[a]
                    helper = fname(a) + '_setjmp'
                    continuation = fname(owner) if owner == a + 4 else fname(owner) + '_resume'
                    out.insert(0, f'void {helper}(Cpu *c);')
                    refs.add(target)
                    jump_helpers += [f'void {continuation}(Cpu *c);',
                        'void rt_guest_setjmp(Cpu *, GuestFn, GuestFn, uint64_t);',
                        f'void {helper}(Cpu *c) {{ rt_guest_setjmp(c, {fname(target)}, '
                        f'{continuation}, {hx(a + 4)}); }}']
                    out.append(f'    __attribute__((musttail)) return {helper}(c);')
                    last = 'b'
                    continue
                h = self.direct_hle.get(t)
                if h is not None:
                    out.append(f'    c->x[30] = {hx(a + 4)}; {sanitize(h)}(c);')
                elif self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    {{ static RtCallCache ic; c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch_cached(c, &ic); }}')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        out += jump_helpers
        self._refs = refs
        return out


from patches import BUILD_ADDRS, EEVEE_100_BUILD, PATCHES

import json as _json
import os as _os
_rv = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'rando', 'patch_variants.json')
RANDO_CODE = {}
if _os.path.exists(_rv):
    for _a, _ws in _json.load(open(_rv))['code'].items():
        RANDO_CODE[int(_a, 16)] = [int(_w, 16) for _w in _ws]


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--always-inline-bytes', type=int, default=96,
                    help='force-inline lifted non-resumable functions up to this many guest text bytes (0 disables)')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--extra-entry', action='append', default=[],
                    help='additional absolute guest code entry (repeatable; hex accepted)')
    ap.add_argument('--extra-entry-range', action='append', default=[],
                    help='additional callable range start:end; every aligned instruction becomes an entry')
    ap.add_argument('--indirect-resume', action='store_true',
                    help='emit static interior-PC resume ranges for indirect-branch unwind intervals')
    ap.add_argument('--setjmp-target', action='append', default=[], type=lambda s: int(s, 0),
                    help='known setjmp thunk (absolute address); used for synthetic tests/other verified builds')
    ap.add_argument('--trace-stores', action='store_true')  # research build (docs/DEPURACAO.md, LARECOMP_TRACE_STORES)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd, args.trace_stores)
    m.always_inline_bytes = max(0, args.always_inline_bytes)
    m.setjmp_targets.update(args.setjmp_target)
    if any(not m.in_text(a) for a in m.setjmp_targets):
        raise SystemExit('setjmp target outside module text')
    bid = m.nso.build_id[:16]
    if args.name == 'main':
        m.direct_guest = {args.base + off: target for off, target in SCARLET_MAIN_DIRECT_GUEST.get(bid, {}).items()}
    if args.name == 'main' and bid != EEVEE_100_BUILD:
        # The instruction patches are written for Let's Go Eevee 1.0.0's main: moved to the same instructions of a
        # build with an address table (tools/patches.py), left out of any other (another update has other code there).
        moved = {BUILD_ADDRS[bid][a]: fn for a, fn in PATCHES.items()} if bid in BUILD_ADDRS else {}
        print(f'{args.name}: build {bid}, ' + (f'{len(moved)} instruction patches moved' if moved else 'no instruction patches'))
        PATCHES.clear()
        PATCHES.update(moved)
        RANDO_CODE.clear()
    starts = m.boundaries()
    hle = {}
    if args.name == 'main':
        for off, name in SCARLET_MAIN_DIRECT_HLE.get(bid, {}).items():
            hle[args.base + off] = name
    want = set()
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Remove the PLT/GOT + indirect dispatcher trip for every standard import
    # we can resolve statically. HLE imports call their host implementation
    # directly; unique exports in sibling NSOs call the lifted target directly.
    # This is especially important for Scarlet, whose slow scenes execute tens
    # of thousands of tiny SDK/import thunks per frame.
    auto_hle = auto_guest = 0
    sibling = sibling_export_targets(args.nso, args.name)
    for entry, name in detect_plt_imports(m.nso, args.base).items():
        if name in want:
            if entry not in hle:
                hle[entry] = name
                auto_hle += 1
            continue
        target = sibling.get(name)
        if target is not None and entry not in m.direct_guest:
            m.direct_guest[entry] = target
            auto_guest += 1
    # Freeze the final HLE map onto the module so every statically-known BL can
    # call host C directly instead of bouncing through a generated wrapper.
    m.direct_hle = dict(hle)
    if auto_hle or auto_guest:
        print(f'{args.name}: direct PLT bindings: {auto_hle} HLE, {auto_guest} sibling guest')

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for raw in args.extra_entry:
        a = int(raw, 0)
        if not m.in_text(a):
            raise SystemExit(f'--extra-entry {raw}: outside {args.name} .text')
        start_set.add(a)
    for raw in args.extra_entry_range:
        try:
            lo_s, hi_s = raw.split(':', 1)
            lo, hi = int(lo_s, 0), int(hi_s, 0)
        except Exception:
            raise SystemExit(f'--extra-entry-range {raw}: expected start:end')
        if lo > hi or (lo & 3) or (hi & 3) or not m.in_text(lo) or not m.in_text(hi):
            raise SystemExit(f'--extra-entry-range {raw}: invalid/outside {args.name} .text')
        start_set.update(range(lo, hi + 4, 4))
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    m.prepare_setjmp(entries)
    resume_entries = m.indirect_resume_entries(entries, hle) if args.indirect_resume or m.setjmp_sites else set()
    m.stats['resume_fragments'] = len(resume_entries)
    m.stats['setjmp_sites'] = len(m.setjmp_sites)
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.limit:
        order = order[:args.limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines = m.emit_function(a, end, labels, tables, hle, a in resume_entries)
                r = getattr(m, '_refs', set())
                m._refs = set()
            n = sum(len(l) + 1 for l in lines)
            if n > args.big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > args.big_fn:
                break
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        # Direct BL sites can reference HLE targets whose wrapper entry lives in
        # another generated chunk, so declare the host implementations globally.
        text += [f'void {sanitize(name)}(Cpu *c);\n' for name in sorted(set(hle.values()))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d{3})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "gamelib.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        resumes = sorted(set(order) & resume_entries)
        for a in resumes:
            f.write(f'void {fname(a)}_resume(Cpu *c);\n')
        f.write(f'const GameLibResumeRange {args.name}_resume_ranges[] = {{\n')
        for a in resumes:
            f.write(f'    {{{hx(a)}, {hx(entries[a][0])}, {fname(a)}_resume}},\n')
        if not resumes:
            f.write('    {0, 0, 0},\n')
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_resume_count = {len(resumes)};\n')
        f.write(f'const uint64_t {args.name}_setjmp_site_count = {len(m.setjmp_sites)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()

