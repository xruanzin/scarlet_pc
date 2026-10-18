#!/usr/bin/env python3
"""Exercise the production Makefile DLL rule with a Windows-sized object list.

No game files needed. Linux test uses GNU make and Clang or Zig to link a real
shared library, with a compiler wrapper enforcing the Windows argv limit.
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


def compiler_command():
    if shutil.which('clang'):
        return ['clang']
    if shutil.which('zig'):
        return ['zig', 'cc']
    if importlib.util.find_spec('ziglang'):
        return [sys.executable, '-m', 'ziglang', 'cc']
    raise unittest.SkipTest('Clang or Zig is required.')


WRAPPER = '''import json, os, subprocess, sys
from pathlib import Path
args = sys.argv[1:]
with open('compiler_calls.jsonl', 'a') as f:
    f.write(json.dumps(args) + '\\n')
if len(subprocess.list2cmdline(args)) > 32767:
    sys.exit('Argument list too long')
if os.environ.get('SCARLET_TEST_LINK_FAIL'):
    Path(args[args.index('-o') + 1]).write_bytes(b'incomplete link')
    sys.exit(1)
# Make exports its command-line CC; Zig's host-libc probes must not recurse
# through this wrapper when discovering the system compiler.
env = os.environ.copy()
env.pop('CC', None)
sys.exit(subprocess.call(json.loads(os.environ['SCARLET_TEST_CC']) + args, env=env))
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('make'),
                     'Requires Linux and GNU make.')
class GameLibraryLinkTests(unittest.TestCase):
    def test_long_object_list_failure_retry_and_incremental_reuse(self):
        cc = compiler_command()
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            build = work / 'build'
            objs = build / 'obj' / 'main'
            objs.mkdir(parents=True)
            (work / 'empty.c').write_text('/* synthetic empty object */\n')
            (work / 'probe.c').write_text('int scarlet_link_probe(void) { return 42; }\n')
            subprocess.run(cc + ['-fPIC', '-c', 'empty.c', '-o', 'empty.o'],
                           cwd=work, check=True, capture_output=True, text=True)
            objects = []
            for i in range(1800):
                rel = f'build/obj/main/main_{i:04d}_synthetic_payload.o'
                os.link(work / 'empty.o', work / rel)
                objects.append(rel)
            objects.append('build/obj/main/probe.o')
            subprocess.run(cc + ['-fPIC', '-c', 'probe.c', '-o', objects[-1]],
                           cwd=work, check=True, capture_output=True, text=True)
            self.assertGreater(len(subprocess.list2cmdline(objects)), 32767)
            before = {p: ((work / p).stat().st_mtime_ns, (work / p).read_bytes()) for p in objects}
            (work / 'compiler.py').write_text(WRAPPER)
            (build / 'lasrecomp').touch()  # order-only runtime prerequisite
            library = build / 'lasgame.so'
            library.write_bytes(b'previous working DLL')
            identity = build / 'lasgame.so.id'
            identity.write_text('previous-id\n')
            # Force a pending link, like the user's newly compiled main objects.
            os.utime(library, ns=(1, 1))
            cmd = ['make', '-f', str(ROOT / 'Makefile'), 'PLATFORM=linux',
                   'B=build', 'GEN=generated', 'GEN_OBJ=' + ' '.join(objects),
                   'RT_OBJ=', 'BACKEND_OBJ=', 'UI_OBJ=', 'IMGUI_OBJ=',
                   'GAMELIB_ID=fixture-id', 'GAMELIB_LDFLAGS=-shared',
                   'CC=' + shlex.join([sys.executable, str(work / 'compiler.py')]),
                   'build/lasgame.so']
            env = os.environ.copy()
            env['SCARLET_TEST_CC'] = json.dumps(cc)
            env['SCARLET_TEST_LINK_FAIL'] = '1'
            failed = subprocess.run(cmd, cwd=work, env=env, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(library.read_bytes(), b'previous working DLL')
            self.assertEqual(identity.read_text(), 'previous-id\n')
            rsp = build / 'lasgame.so.rsp'
            self.assertEqual(shlex.split(rsp.read_text()), objects)

            del env['SCARLET_TEST_LINK_FAIL']
            linked = subprocess.run(cmd, cwd=work, env=env, capture_output=True, text=True)
            self.assertEqual(linked.returncode, 0, linked.stdout + linked.stderr)
            self.assertEqual(ctypes.CDLL(str(library)).scarlet_link_probe(), 42)
            self.assertEqual(identity.read_text(), 'fixture-id\n')
            calls = [json.loads(line) for line in (work / 'compiler_calls.jsonl').read_text().splitlines()]
            self.assertEqual(len(calls), 2, calls)
            for args in calls:
                self.assertEqual(args, ['-shared', '-o', 'build/lasgame.so.tmp', '@build/lasgame.so.rsp'])

            again = subprocess.run(cmd, cwd=work, env=env, capture_output=True, text=True)
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertEqual(len((work / 'compiler_calls.jsonl').read_text().splitlines()), 2)
            for p, (mtime, data) in before.items():
                self.assertEqual((work / p).stat().st_mtime_ns, mtime)
                self.assertEqual((work / p).read_bytes(), data)


if __name__ == '__main__':
    unittest.main(verbosity=2)
