param(
    [string]$SourceDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'launcher'),
    [string]$BuildDir  = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\scarlet-launcher'),
    [string]$IconPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) {
        throw 'Scarlet Launcher requires the MSVC C++ Build Tools, but vswhere.exe was not found.'
    }

    $vsRoot = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($vsRoot)) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }

    $vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat not found at $vsDevCmd"
    }

    Write-Host '[Scarlet PC] Loading MSVC x64 environment for launcher...' -ForegroundColor DarkGray
    $envLines = & cmd.exe /s /c "`"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $envLines) {
        $p = $line.IndexOf('=')
        if ($p -gt 0) {
            Set-Item -Path ("Env:" + $line.Substring(0, $p)) -Value $line.Substring($p + 1)
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'MSVC environment loaded but cl.exe is still unavailable.'
    }
}

Import-MsvcEnvironment

$repoRoot = Split-Path $PSScriptRoot -Parent
$source = Join-Path $SourceDir 'ScarletLauncher.cpp'
$background = Join-Path $repoRoot 'docs\viridian.jpg'
if (-not (Test-Path $source -PathType Leaf)) {
    throw "Launcher source not found: $source"
}
if (-not (Test-Path $background -PathType Leaf)) {
    throw "High-resolution launcher background not found: $background"
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$stage = Join-Path $BuildDir 'stage'
$stageAssets = Join-Path $stage 'assets'
New-Item -ItemType Directory -Force -Path $stageAssets | Out-Null

$resource = $null
if (-not [string]::IsNullOrWhiteSpace($IconPath) -and (Test-Path $IconPath -PathType Leaf)) {
    $rc = Get-Command rc.exe -ErrorAction SilentlyContinue
    if ($rc) {
        $resourceScript = Join-Path $BuildDir 'ScarletLauncher.rc'
        $resource = Join-Path $BuildDir 'ScarletLauncher.res'
        $escapedIcon = ([IO.Path]::GetFullPath($IconPath)).Replace('\', '\\')
        "1 ICON `"$escapedIcon`"" | Set-Content $resourceScript -Encoding ASCII
        & $rc.Source /nologo "/fo$resource" $resourceScript
        if ($LASTEXITCODE -ne 0) {
            throw 'Scarlet Launcher resource compilation failed.'
        }
    }
}

$exe = Join-Path $BuildDir 'ScarletLauncher.exe'
$args = @(
    '/nologo',
    '/std:c++20',
    '/O2',
    '/EHsc',
    '/utf-8',
    '/DUNICODE',
    '/D_UNICODE',
    ("/Fo:{0}" -f (Join-Path $BuildDir 'ScarletLauncher.obj')),
    $source
)
if ($resource) { $args += $resource }
$args += @(
    "/Fe:$exe",
    '/link',
    '/SUBSYSTEM:WINDOWS',
    'user32.lib',
    'gdi32.lib',
    'gdiplus.lib',
    'comctl32.lib',
    'comdlg32.lib',
    'dwmapi.lib',
    'shell32.lib',
    'shlwapi.lib',
    'uxtheme.lib',
    'ole32.lib'
)

Write-Host '[Scarlet PC] Building native launcher...' -ForegroundColor Cyan
$log = Join-Path $BuildDir 'scarlet_launcher_build.log'
& cl.exe @args 2>&1 | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) {
    Write-Host '[Scarlet PC] Launcher build failed. Last compiler messages:' -ForegroundColor Red
    Get-Content $log -Tail 80
    throw 'Scarlet Launcher build failed.'
}

Copy-Item $exe (Join-Path $stage 'ScarletLauncher.exe') -Force
Copy-Item $background (Join-Path $stageAssets 'scarlet_launcher_background.jpg') -Force

[ordered]@{
    project = 'Scarlet PC'
    component = 'launcher'
    executable = 'ScarletLauncher.exe'
    background = 'assets/scarlet_launcher_background.jpg'
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content (Join-Path $stage 'scarlet_launcher_manifest.json') -Encoding UTF8

Write-Host "[Scarlet PC] Launcher ready: $stage\ScarletLauncher.exe" -ForegroundColor Green
