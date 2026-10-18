#!/usr/bin/env python3
"""Run scarlet_run.ps1 with a fake native game in PowerShell; no Windows/game required.

PWSH selects PowerShell 7 when it is not on PATH. Only the native process and
the external MSYS installation probe are mocked; environment, paths and ZIPs are real.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = r'''
param([string]$Repo, [string]$Game, [int]$WantedExit, [int]$WithShader, [int]$WithGraphics = 0, [int]$WithProfile = 0)
$ErrorActionPreference = 'Stop'
$Runner = Join-Path $Repo 'tools/scarlet_run.ps1'
$Tokens = $null; $Errors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile($Runner, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw ($Errors | Out-String) }
$global:FixtureRepo = $Repo
$global:FixtureDataRoot = Join-Path $Repo 'build'
$global:FixtureExit = $WantedExit
$global:FixtureShader = $WithShader
$global:FixtureCalls = 0
$global:FixtureGraphics = $WithGraphics
$global:FixtureProfile = $WithProfile
function Test-Path {
    [CmdletBinding()] param([string[]]$LiteralPath, [string[]]$Path, [Alias('Type')][string]$PathType)
    if ($LiteralPath -eq 'C:\msys64\ucrt64\bin') { return $true }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
function Invoke-FakeScarlet {
    $global:FixtureCalls++
    if ((Get-Location).Path -ne $global:FixtureRepo) { throw 'runner did not anchor paths' }
    if ($env:LARECOMP_VK_OPT -ne '0') { throw 'runner inherited experimental shader optimization' }
    if ($global:FixtureProfile) {
        if ($env:LARECOMP_VK_PROF -ne '1' -or $env:LARECOMP_HLE_PROF -ne '1') { throw 'profiling not enabled' }
    }
    elseif ($env:LARECOMP_VK_PROF -or $env:LARECOMP_HLE_PROF) { throw 'runner inherited profiling' }
    if ($global:FixtureGraphics) {
        if (-not [IO.Path]::IsPathRooted($env:LARECOMP_TF_DIAG_DIR)) { throw 'diagnostic path is not absolute' }
        [IO.File]::WriteAllBytes((Join-Path $env:LARECOMP_TF_DIAG_DIR 'shader_test.control.bin'), [byte[]](1,2,3,4))
    }
    elseif ($env:LARECOMP_TF_DIAG_DIR) { throw 'runner inherited shader dumps' }
    $Text = '[paths] data: ' + $global:FixtureDataRoot + "`n[fake] boot"
    if ($global:FixtureGraphics) {
        $Dir = Join-Path $global:FixtureDataRoot 'build/shader-diagnostics'
        $null = New-Item -ItemType Directory -Force -Path $Dir
        [IO.File]::WriteAllText((Join-Path $Dir 'nopos_70000100_vs.glsl'), 'current-nopos')
        [IO.File]::WriteAllBytes((Join-Path $Dir 'nopos_70000100_vs.sass.bin'), [byte[]](5,6,7,8))
        [IO.File]::WriteAllText((Join-Path $Dir 'nopos_deadbeef_vs.glsl'), 'stale-nopos')
        $Text += "`n[shader] VS has no position export: code=70000100 source=13 bytes inst=2 cases=0"
    }
    if ($global:FixtureShader) {
        $Dir = Join-Path $global:FixtureDataRoot 'build/shader-diagnostics'
        $null = New-Item -ItemType Directory -Force -Path $Dir
        [IO.File]::WriteAllText((Join-Path $Dir '0123456789abcdef.frag.glsl'), 'current-source')
        [IO.File]::WriteAllBytes((Join-Path $Dir '0123456789abcdef.frag.spv'), [byte[]](1,2,3,4))
        [IO.File]::WriteAllText((Join-Path $Dir 'stale.frag.glsl'), 'stale-source')
        $Line = '[shader-diagnostic] input build/shader-diagnostics/0123456789abcdef.frag.glsl (14 bytes), SPIR-V=1 words'
        $Text += "`n$Line`n$Line"
        # A deadline may interrupt shaderc before any SPIR-V was written.
        [IO.File]::WriteAllText((Join-Path $Dir 'fedcba9876543210.comp.glsl'), 'pending-source')
        $Text += "`n[shader-diagnostic] input build/shader-diagnostics/fedcba9876543210.comp.glsl (14 bytes), SPIR-V=0 words"
        $Text += "`n[shader-slow] pipeline still compiling after 3.0 s"
    }
    if ($global:FixtureExit -eq 86) { $Text += "`n[build-timeout] pipeline exceeded 8 s" }
    [IO.File]::WriteAllText((Join-Path $global:FixtureRepo 'scarlet_boot.log'), $Text)
    $global:LASTEXITCODE = $global:FixtureExit
}
$env:ComSpec = 'Invoke-FakeScarlet'
$env:LARECOMP_VK_OPT = '1'
$env:LARECOMP_VK_PROF = '1'
$env:LARECOMP_HLE_PROF = 'old-hle'
$env:LARECOMP_TF_DIAG_DIR = 'old-dir'
$Before = (Get-Location).Path
& $Runner -GameRoot $Game -GraphicsDiagnostics:([bool]$WithGraphics) -Profile:([bool]$WithProfile)
if ($LASTEXITCODE -ne $WantedExit) { throw "exit code changed to $LASTEXITCODE" }
if ($global:FixtureCalls -ne 1) { throw 'game not called exactly once' }
if ((Get-Location).Path -ne $Before) { throw 'working directory was not restored' }
if ($env:LARECOMP_VK_OPT -ne '1' -or $env:LARECOMP_VK_PROF -ne '1') { throw 'environment was not restored' }
if ($env:LARECOMP_HLE_PROF -ne 'old-hle' -or $env:LARECOMP_TF_DIAG_DIR -ne 'old-dir') { throw 'diagnostic environment not restored' }
'''


class RunnerTests(unittest.TestCase):
    def test_real_runner_environment_exit_and_diagnostic_archive(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:
            self.skipTest('requires PowerShell 7 (PWSH)')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = root / 'fixture.ps1'
            fixture.write_text(SCRIPT)
            for code, shaders, graphics, profile in ((0, False, False, False), (0, True, False, False),
                    (86, False, False, False), (86, True, False, False),
                    (0, False, True, True), (86, False, True, True), (0, False, True, False)):
                with self.subTest(code=code, shaders=shaders, graphics=graphics, profile=profile):
                    repo = root / f'repo_{code}_{shaders}_{graphics}_{profile}'
                    game = root / 'game'
                    for directory in (repo / 'tools', repo / 'build', game / 'exefs', game / 'romfs'):
                        directory.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(ROOT / 'tools/scarlet_run.ps1', repo / 'tools/scarlet_run.ps1')
                    for path in (repo / 'build/ScarletRecomp.exe', repo / 'build/lasgame.dll',
                                 game / 'exefs/main', game / 'exefs/main.npdm'):
                        path.touch()
                    run = subprocess.run([pwsh, '-NoLogo', '-NoProfile', '-File', str(fixture),
                                          str(repo), str(game), str(code), str(int(shaders)),
                                          str(int(graphics)), str(int(profile))],
                                         cwd=root, capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    bundles = list((repo / 'build').glob('scarlet-diagnostics-*.zip'))
                    self.assertEqual(len(bundles), int(graphics), run.stdout + run.stderr)
                    if graphics:
                        with zipfile.ZipFile(bundles[0]) as z:
                            self.assertEqual(set(z.namelist()), {'shader_test.control.bin', 'scarlet_boot.log', 'run.json',
                                'nopos_70000100_vs.glsl', 'nopos_70000100_vs.sass.bin'})
                            self.assertEqual(z.read('shader_test.control.bin'), bytes([1, 2, 3, 4]))
                            self.assertEqual(z.read('scarlet_boot.log'), (repo / 'scarlet_boot.log').read_bytes())
                    archive = repo / 'build/scarlet_shader_failure.zip'
                    self.assertEqual(archive.exists(), code == 86 or shaders, run.stdout + run.stderr)
                    if code != 86 and not shaders:
                        continue
                    with zipfile.ZipFile(archive) as z:
                        expected = {'scarlet_boot.log'}
                        if shaders:
                            expected |= {'0123456789abcdef.frag.glsl', '0123456789abcdef.frag.spv',
                                         'fedcba9876543210.comp.glsl'}
                        self.assertEqual(set(z.namelist()), expected)
                        self.assertEqual(len(z.namelist()), len(expected))
                        self.assertEqual(z.read('scarlet_boot.log'), (repo / 'scarlet_boot.log').read_bytes())
                        if shaders:
                            self.assertEqual(z.read('0123456789abcdef.frag.glsl'), b'current-source')


if __name__ == '__main__':
    unittest.main(verbosity=2)
