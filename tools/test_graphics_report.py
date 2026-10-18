#!/usr/bin/env python3
"""Synthetic NSO/PE tests for the read-only graphics report; no game files."""
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from scarlet_graphics_report import crash_details, make_report, native_context, pe_function, pe_layout, pe_offset, read_log
from test_indirect_resume import write_fixture


def write_pe(path):
    image = bytearray(0x600)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3C, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', image, 0x84, 0x8664, 2)
    struct.pack_into('<H', image, 0x94, 0xF0)
    opt = 0x98
    struct.pack_into('<H', image, opt, 0x20B)
    struct.pack_into('<I', image, opt + 108, 16)
    struct.pack_into('<II', image, opt + 112 + 3 * 8, 0x2000, 12)
    for i, (name, rva, raw) in enumerate(((b'.text', 0x1000, 0x200), (b'.pdata', 0x2000, 0x400))):
        at = opt + 0xF0 + i * 40
        image[at:at + len(name)] = name
        struct.pack_into('<IIII', image, at + 8, 0x200, rva, 0x200, raw)
    # mov rax,[rcx+0xe8]; mov rax,[rax]; ret -- exact fault is the second load.
    code = bytes.fromhex('488b81e8000000488b00c3')
    image[0x200:0x200 + len(code)] = code
    struct.pack_into('<III', image, 0x400, 0x1000, 0x1000 + len(code), 0)
    path.write_bytes(image)
    return image


LOG = '''[dispatch] main: 7 static resume ranges
[gfx-milestone] nvnBootstrapLoader reached
*** exception c0000005 accessing 0xffffffffffffffff
    host pc 180001007 (lasgame.dll+1007)
  nvnSamplerInitialize x0=7274470d40 x1=723ffff430 lr=700000022c
host backtrace (guest functions are f_<addr>):
  #0  0000000180001007 f_7000000100+7
guest: sp=723ffff4a0 lr=0 tpidr=7240001140 tpidrro=7090000000
  x28 0000007246649b20  x29 3f8000003f800000  x30 0000000000000000
'''


class GraphicsReportTests(unittest.TestCase):
    def test_pe_rva_mapping_and_exact_fault_instruction(self):
        with tempfile.TemporaryDirectory() as temp:
            dll = Path(temp) / 'game.dll'
            image = write_pe(dll)
            sections, exception = pe_layout(image)
            self.assertEqual(pe_offset(sections, 0x1007, 3), 0x207)
            self.assertEqual(pe_function(image, sections, exception, 0x1007), (0x1000, 0x100B))
            self.assertIsNone(pe_function(image, sections, exception, 0x1010))
            with self.assertRaises(ValueError):
                pe_offset(sections, 0x1FFF)
            fault, frames, calls = crash_details(LOG)
            out = io.StringIO()
            native_context(out, dll, fault, frames)
            self.assertIn('=> RVA 0x1007: 488b00', out.getvalue())
            self.assertIn('mov      rax, qword ptr [rax]', out.getvalue())
            self.assertEqual(calls[0][0], 'nvnSamplerInitialize')

    def test_report_reads_utf16_and_preserves_all_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            inputs = []
            for name in ('main', 'sdk', 'subsdk0'):
                inputs.append(exefs / name)
                write_fixture(inputs[-1])
            dll, log = root / 'lasgame.dll', root / 'boot.log'
            write_pe(dll)
            log.write_text(LOG, encoding='utf-16')
            inputs += [dll, log]
            before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
            self.assertEqual(read_log(log), LOG)
            report, missing = make_report(exefs, log, dll)
            self.assertEqual(missing, [])
            self.assertIn('0x7000000100:', report)
            self.assertIn('C: c->sp', report)
            self.assertIn('3f8000003f800000', report)
            self.assertIn('=> RVA 0x1007:', report)
            self.assertIn('not proof that the last call caused', report)
            self.assertEqual(before, {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs})

    def test_missing_and_mismatched_native_image_are_explicit(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            write_fixture(exefs / 'main')
            log, dll = root / 'boot.log', root / 'game.dll'
            log.write_text(LOG, encoding='utf-8')
            report, missing = make_report(exefs, log, dll)
            self.assertEqual(missing, ['sdk', 'subsdk0'])
            self.assertIn('[native] missing DLL', report)
            image = write_pe(dll)
            image[0x200:0x20B] = b'\x90' * 11
            image[0x206:0x208] = bytes.fromhex('6690')  # 1007 is inside an instruction
            dll.write_bytes(image)
            report, _ = make_report(exefs, log, dll)
            self.assertIn('fault is not an instruction boundary', report)

    def test_cli_writes_only_text_and_refuses_input_overwrites(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            exefs = root / 'exefs'
            exefs.mkdir()
            for name in ('main', 'sdk', 'subsdk0'):
                write_fixture(exefs / name)
            dll, log, output = root / 'game.dll', root / 'boot.txt', root / 'report.txt'
            write_pe(dll)
            log.write_text(LOG)
            inputs = [exefs / 'main', dll, log]
            before = [p.read_bytes() for p in inputs]
            command = [sys.executable, str(Path(__file__).with_name('scarlet_graphics_report.py')),
                       str(exefs), '--log', str(log), '--dll', str(dll), '--output']
            good = subprocess.run(command + [str(output)], capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stderr)
            self.assertIn('=> RVA 0x1007:', output.read_text())
            for unsafe in [exefs / 'main', exefs / 'new.txt', dll, log, root / 'bad.dll']:
                run = subprocess.run(command + [str(unsafe)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 2, run.stderr)
            alias = root / 'dll_alias.txt'
            os.link(dll, alias)
            run = subprocess.run(command + [str(alias)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 2)
            game_alias = root / 'main_alias.txt'
            os.link(exefs / 'main', game_alias)
            run = subprocess.run(command + [str(game_alias)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 2)
            self.assertEqual([p.read_bytes() for p in inputs], before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
