param(
    [Parameter(Mandatory = $true)]
    [string]$ExportRoot,
    [string]$AotDir,
    [ValidateRange(1, 4)]
    [int]$Jobs = 2,
    [switch]$Fresh,
    [switch]$Repackage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($AotDir)) {
    $AotDir = Join-Path $repoRoot 'build\aot\modules'
}

$export = (Resolve-Path $ExportRoot).Path
$recompRoot = Join-Path $export 'aot_cache\exefs'
if (-not (Test-Path $recompRoot -PathType Container)) {
    throw "Scarlet AOT source tree not found: $recompRoot"
}

function Get-VisualStudioRoot {
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ([string]::IsNullOrWhiteSpace($pf86)) { return $null }
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) { return $null }
    $root = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($root)) { return $null }
    return $root.Trim()
}

function Import-VisualStudioEnvironment([string]$VsRoot) {
    if ([string]::IsNullOrWhiteSpace($VsRoot)) { return }
    $devCmd = Join-Path $VsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devCmd -PathType Leaf)) { return }
    Write-Host '[Scarlet PC] Loading Visual Studio x64 build environment...' -ForegroundColor DarkGray
    $command = ('"{0}" -no_logo -arch=x64 -host_arch=x64 >nul && set' -f $devCmd)
    $lines = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "VsDevCmd failed with exit code $LASTEXITCODE" }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -le 0) { continue }
        Set-Item -Path ('Env:' + $line.Substring(0, $eq)) -Value $line.Substring($eq + 1)
    }
}

function Find-ClangCl([string]$VsRoot) {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($envName in @('SCARLET_CLANG_CL', 'SUYU_CLANG_CL')) {
        $value = [Environment]::GetEnvironmentVariable($envName)
        if (-not [string]::IsNullOrWhiteSpace($value)) { $candidates.Add($value) }
    }
    if ($env:ProgramFiles) { $candidates.Add((Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe')) }
    if (-not [string]::IsNullOrWhiteSpace($VsRoot)) {
        $candidates.Add((Join-Path $VsRoot 'VC\Tools\Llvm\x64\bin\clang-cl.exe'))
        $candidates.Add((Join-Path $VsRoot 'VC\Tools\Llvm\bin\clang-cl.exe'))
    }
    $cmd = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add($cmd.Source) }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path $candidate -PathType Leaf)) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function Find-CMake([string]$VsRoot) {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (-not [string]::IsNullOrWhiteSpace($VsRoot)) {
        $candidate = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

function Find-Ninja([string]$VsRoot) {
    $cmd = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (-not [string]::IsNullOrWhiteSpace($VsRoot)) {
        $candidate = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

$vsRoot = Get-VisualStudioRoot
Import-VisualStudioEnvironment $vsRoot
$clang = Find-ClangCl $vsRoot
if (-not $clang) {
    throw 'clang-cl not found. Install the Visual Studio component C++ Clang tools for Windows or LLVM for Windows, then rerun this same command. You may also set SCARLET_CLANG_CL to clang-cl.exe.'
}
$cmake = Find-CMake $vsRoot
if (-not $cmake) { throw 'cmake.exe was not found.' }
$ninja = Find-Ninja $vsRoot
if (-not $ninja) { throw 'ninja.exe was not found.' }

$previousEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $clangOutput = @(& $clang --version 2>&1)
    $clangExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousEap
}
if ($clangExit -ne 0) {
    $detail = ($clangOutput | Out-String).Trim()
    throw "clang-cl self-test failed with exit code $clangExit using '$clang'. $detail"
}
$clangVersion = ($clangOutput | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($clangVersion)) { $clangVersion = 'clang-cl' }

Write-Host ''
Write-Host '=== Scarlet PC AOT clang-cl performance rebuild ===' -ForegroundColor Cyan
Write-Host "[Scarlet PC] Export: $export" -ForegroundColor DarkGray
Write-Host "[Scarlet PC] Compiler: $clangVersion" -ForegroundColor Green
Write-Host "[Scarlet PC] AOT compile jobs: $Jobs" -ForegroundColor DarkGray

$modules = @(
    [pscustomobject]@{ Name = 'main'; Dll = 'recompiled_image.dll'; Target = 'recompiled_image' },
    [pscustomobject]@{ Name = 'rtld'; Dll = 'recompiled_rtld.dll'; Target = 'recompiled_rtld' },
    [pscustomobject]@{ Name = 'subsdk0'; Dll = 'recompiled_subsdk0.dll'; Target = 'recompiled_subsdk0' },
    [pscustomobject]@{ Name = 'sdk'; Dll = 'recompiled_sdk.dll'; Target = 'recompiled_sdk' }
)

New-Item -ItemType Directory -Force -Path $AotDir | Out-Null
$aotOut = (Resolve-Path $AotDir).Path
$manifestModules = @()

foreach ($module in $modules) {
    $sourceDir = Join-Path $recompRoot $module.Name
    if (-not (Test-Path (Join-Path $sourceDir 'CMakeLists.txt') -PathType Leaf)) {
        throw "Generated AOT project is missing for $($module.Name): $sourceDir"
    }

    $buildDir = Join-Path $sourceDir 'build-clang'
    if ($Fresh -and (Test-Path $buildDir -PathType Container)) {
        Remove-Item $buildDir -Recurse -Force
    }
    if (Test-Path (Join-Path $buildDir 'CMakeCache.txt') -PathType Leaf) {
        $cache = (Get-Content (Join-Path $buildDir 'CMakeCache.txt') -Raw).Replace('\', '/')
        $normalizedClang = $clang.Replace('\', '/')
        if (-not $cache.Contains($normalizedClang)) {
            Write-Host "[Scarlet PC] Compiler changed; resetting $($module.Name) clang cache." -ForegroundColor Yellow
            Remove-Item $buildDir -Recurse -Force
        }
    }

    Write-Host ''
    Write-Host "[Scarlet PC] Configuring AOT $($module.Name) with clang-cl /O2..." -ForegroundColor Cyan
    $configureArgs = @(
        '-G', 'Ninja',
        '-S', $sourceDir,
        '-B', $buildDir,
        '-DCMAKE_BUILD_TYPE=Release',
        '-DRECOMP_BUILD_STATIC_LIB=OFF',
        "-DRECOMP_JOBS=$Jobs",
        "-DCMAKE_C_COMPILER=$clang",
        "-DCMAKE_MAKE_PROGRAM=$ninja"
    )
    & $cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $($module.Name) with exit code $LASTEXITCODE" }

    Write-Host "[Scarlet PC] Building AOT $($module.Name)..." -ForegroundColor Cyan
    $buildArgs = @('--build', $buildDir, '--target', $module.Target, '--parallel', "$Jobs")
    & $cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "clang-cl AOT build failed for $($module.Name) with exit code $LASTEXITCODE" }

    $dll = Get-ChildItem $buildDir -Recurse -File -Filter $module.Dll | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $dll) { throw "clang-cl build completed but $($module.Dll) was not found under $buildDir" }

    $destination = Join-Path $aotOut $module.Dll
    Copy-Item $dll.FullName $destination -Force
    $copied = Get-Item $destination
    $manifestModules += [ordered]@{
        name = $module.Dll
        module = $module.Name
        bytes = $copied.Length
        source = $dll.FullName
        compiler = $clangVersion
        build_tree = $buildDir
    }
    Write-Host ("[OK] {0,-26} {1,8:N1} MB" -f $module.Dll, ($copied.Length / 1MB)) -ForegroundColor Green
}

[ordered]@{
    format = 3
    project = 'Scarlet PC'
    title_id = '0100A3D008C5C000'
    architecture = 'x86-64'
    backend = 'Static/Hybrid AOT shared modules'
    compiler_strategy = 'clang-cl /O2 performance rebuild'
    compiler = $clangVersion
    compiler_path = $clang
    export_root = $export
    generated_at_utc = [DateTime]::UtcNow.ToString('o')
    modules = $manifestModules
} | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $aotOut 'scarlet-aot-manifest.json') -Encoding UTF8

if ($Repackage) {
    Write-Host ''
    Write-Host '[Scarlet PC] Refreshing independent package with clang-cl AOT DLLs...' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'scarlet_independent_package.ps1') -AotDir $aotOut
    if ($LASTEXITCODE -ne 0) { throw "Independent package refresh failed with exit code $LASTEXITCODE" }
}

Write-Host ''
Write-Host '[Scarlet PC] clang-cl AOT performance rebuild complete.' -ForegroundColor Green
Write-Host "[Scarlet PC] AOT modules: $aotOut" -ForegroundColor Green
