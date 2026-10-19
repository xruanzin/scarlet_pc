#!/usr/bin/env python3
"""Exercise real PowerShell driver selection/fallback with synthetic PE fixtures."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent.parent
SCRIPT=r'''
param($Helper,$Fixture)
$ErrorActionPreference='Stop'
. $Helper
function Check($Condition,$Message) { if (-not $Condition) { throw $Message } }
$Log=Join-Path $Fixture 'boot.log'
$env:VK_DRIVER_FILES='original-driver'
$env:VK_ICD_FILENAMES='original-legacy'
$env:LARECOMP_GRAPHICS_BACKEND='original-mode'
$env:LARECOMP_GPU='9'
$env:VK_LOADER_DEBUG='original-debug'
Remove-Item Env:LARECOMP_VK_ALLOW_13 -ErrorAction SilentlyContinue
$OriginalPath=$env:PATH
$global:Attempts=0
$Run={ param($Out)
    $global:Attempts++
    Check ($env:LARECOMP_GRAPHICS_BACKEND -eq 'directx-dozen') 'not directx'
    Check ($env:VK_DRIVER_FILES -like '*dzn_icd.x86_64.json') 'driver not selected'
    Check (-not $env:LARECOMP_GPU) 'inherited native GPU index'
    '[graphics-backend] ready=directx-dozen' | Set-Content -LiteralPath $Out
    return 0
}
$Code=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -Run $Run
Check ($Code -eq 0 -and $global:Attempts -eq 1) 'success retry'
Check ($env:VK_DRIVER_FILES -eq 'original-driver' -and $env:VK_ICD_FILENAMES -eq 'original-legacy') 'ICD leak'
Check ($env:PATH -eq $OriginalPath -and $env:LARECOMP_GPU -eq '9') 'PATH/GPU leak'
Check ($env:LARECOMP_GRAPHICS_BACKEND -eq 'original-mode') 'backend leak'
$global:Attempts=0
$Run={param($Out)
    $global:Attempts++
    if ($global:Attempts -eq 1) { 'no Vulkan 1.3 device' | Set-Content -LiteralPath $Out; return 7 }
    Check ($env:LARECOMP_GRAPHICS_BACKEND -eq 'vulkan' -and -not $env:VK_DRIVER_FILES) 'fallback not native'
    Check ($env:PATH -eq $OriginalPath) 'fallback PATH'
    Check ($env:VK_LOADER_DEBUG -eq 'original-debug') 'fallback debug leak'
    Check ($env:LARECOMP_VK_ALLOW_13 -eq '1') 'fallback did not allow the native AMD 1.3 driver'
    '[graphics-backend] ready=vulkan' | Set-Content -LiteralPath $Out
    return 0
}
$Code=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -Run $Run
Check ($Code -eq 0 -and $global:Attempts -eq 2) 'no fallback'
Check (-not $env:LARECOMP_VK_ALLOW_13) 'compatibility flag leaked'
Check ((Get-Content -LiteralPath "$Log.directx.failed.log" -Raw) -match 'no Vulkan') 'lost failure log'
$global:Attempts=0
$Code=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -NoFallback -Run $Run
Check ($Code -eq 7 -and $global:Attempts -eq 1) 'strict mode retried'
$global:Attempts=0
$Code=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -Run {
 param($Out); $global:Attempts++; '[graphics-backend] ready=directx-dozen' | Set-Content $Out; return 9
}
Check ($Code -eq 9 -and $global:Attempts -eq 1) 'restarted after renderer ready'
try { $null=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -Run { throw 'fixture-run-failure' }; throw 'missing exception' }
catch { Check ($_.ToString() -match 'fixture-run-failure') 'wrong exception' }
Check ($env:PATH -eq $OriginalPath -and $env:VK_DRIVER_FILES -eq 'original-driver') 'exception leaked environment'
$Code=Invoke-ScarletGraphicsBackend Vulkan '' $Log -Run {
 param($Out); Check (-not $env:VK_DRIVER_FILES -and -not $env:VK_ICD_FILENAMES) 'Vulkan inherited Dozen'; return 0
}
Check ($Code -eq 0) 'Vulkan failed'
$Missing=Join-Path $Fixture 'missing'
$Code=Invoke-ScarletGraphicsBackend DirectX $Missing $Log -Run {
 param($Out); Check ($env:LARECOMP_GRAPHICS_BACKEND -eq 'vulkan') 'missing package no fallback'; return 0
}
Check ($Code -eq 0) 'missing package fallback failed'
try { $null=Invoke-ScarletGraphicsBackend DirectX $Missing $Log -NoFallback -Run { throw 'should not run' }; throw 'accepted missing driver' }
catch { Check ($_.ToString() -notmatch 'accepted missing driver|should not run') 'strict missing driver executed' }
function Test-ScarletElevated { return $true }
$global:Attempts=0
$Code=Invoke-ScarletGraphicsBackend DirectX $Fixture $Log -Run {
 param($Out); $global:Attempts++; Check ($env:LARECOMP_GRAPHICS_BACKEND -eq 'vulkan') 'elevated DirectX launched'; return 0
}
Check ($Code -eq 0 -and $global:Attempts -eq 1) 'elevated fallback failed'
Check ((Get-Content -LiteralPath "$Log.directx.failed.log" -Raw) -match 'administrador') 'missing elevated explanation'
function Test-ScarletElevated { return $false }
$Dll=Join-Path $Fixture 'vulkan_dzn.dll'
$Bytes=[IO.File]::ReadAllBytes($Dll); $Bytes[68]=0x4c; $Bytes[69]=0x01
[IO.File]::WriteAllBytes($Dll,$Bytes)
try { $null=Get-ScarletDirectXDriver $Fixture; throw 'accepted x86 DLL' }
catch { Check ($_.ToString() -match 'precisa ser Windows x64') 'wrong architecture check' }
'PASS'
'''

class GraphicsBackendTests(unittest.TestCase):
    def test_selection_fallback_and_environment_restoration(self):
        pwsh=os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:self.skipTest('PowerShell 7 required')
        with tempfile.TemporaryDirectory(prefix='scarlet dx ') as tmp:
            root=Path(tmp);dll=bytearray(128)
            struct.pack_into('<H',dll,0,0x5a4d);struct.pack_into('<I',dll,0x3c,64)
            struct.pack_into('<IH',dll,64,0x4550,0x8664)
            (root/'vulkan_dzn.dll').write_bytes(dll)
            (root/'dzn_icd.x86_64.json').write_text(json.dumps({'ICD':{'library_path':'vulkan_dzn.dll','api_version':'1.3.0'}}))
            script=root/'test.ps1';script.write_text(SCRIPT)
            run=subprocess.run([str(Path(pwsh).resolve()),'-NoProfile','-File',str(script),
                str(ROOT/'tools/scarlet_graphics_backend.ps1'),str(root)],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('PASS',run.stdout)
    def test_setup_rejects_unverified_archive(self):
        pwsh=os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:self.skipTest('PowerShell 7 required')
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);archive=root/'bad.7z';archive.write_bytes(b'not-the-publisher-package')
            destination=root/'installed'
            run=subprocess.run([str(Path(pwsh).resolve()),'-NoProfile','-File',
                str(ROOT/'tools/scarlet_setup_directx.ps1'),'-Archive',str(archive),'-Destination',str(destination)],
                capture_output=True,text=True,timeout=20)
            self.assertNotEqual(run.returncode,0)
            self.assertIn('Checksum Mesa incorreto',run.stdout+run.stderr)
            self.assertFalse(destination.exists())

    def test_scripts_parse(self):
        pwsh=os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:self.skipTest('PowerShell 7 required')
        for name in ('scarlet_run.ps1','scarlet_setup_directx.ps1','scarlet_graphics_backend.ps1'):
            path=str(ROOT/'tools'/name).replace("'","''")
            cmd=f"$e=$null; $t=$null; $null=[System.Management.Automation.Language.Parser]::ParseFile('{path}',[ref]$t,[ref]$e); if($e.Count){{$e|Out-String|Write-Error;exit 1}}"
            run=subprocess.run([str(Path(pwsh).resolve()),'-NoProfile','-Command',cmd],capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

if __name__=='__main__':unittest.main(verbosity=2)
