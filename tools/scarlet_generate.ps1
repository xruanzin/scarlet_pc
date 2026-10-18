param(
    [Parameter(Mandatory=$true)]
    [string]$ExeFs,

    [string]$OutDir = "generated",

    [ValidateSet("all", "main", "sdk", "subsdk0")]
    [string]$Module = "all",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ExeFs = (Resolve-Path $ExeFs).Path

if (-not [System.IO.Path]::IsPathRooted($OutDir)) {
    $OutDir = Join-Path $RepoRoot $OutDir
}

$Required = @("main", "sdk", "subsdk0")
foreach ($Name in $Required) {
    $Path = Join-Path $ExeFs $Name
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Modulo ausente no ExeFS: $Path"
    }
}

# Prefer the project venv created by setup_windows.ps1. Falling back to the
# global "python" caused Windows Store / other Python installs to miss the
# generator dependencies even though setup had already installed them.
$VenvPython = Join-Path $RepoRoot ".venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $VenvPython -PathType Leaf)) {
    $BasePython = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
    if (-not (Test-Path -LiteralPath $BasePython -PathType Leaf)) {
        $BasePython = (Get-Command python -ErrorAction Stop).Source
    }

    Write-Host "[scarlet-gen] Criando ambiente Python do gerador..." -ForegroundColor Cyan
    & $BasePython -m venv (Join-Path $RepoRoot ".venv")
    if ($LASTEXITCODE -ne 0) {
        throw "Nao foi possivel criar .venv para o gerador (exit $LASTEXITCODE)"
    }
}
$Python = $VenvPython

# PowerShell 5.1 can promote native stderr to NativeCommandError while
# ErrorActionPreference=Stop. Probe with Continue and inspect LASTEXITCODE.
$PrevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $Python -c "import lz4.block, unicorn, capstone" 2>$null
$DepsCode = $LASTEXITCODE
$ErrorActionPreference = $PrevEap

if ($DepsCode -ne 0) {
    Write-Host "[scarlet-gen] Instalando dependencias Python do gerador..." -ForegroundColor Cyan
    $PrevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Python -m pip install -q lz4 unicorn capstone
    $PipCode = $LASTEXITCODE
    $ErrorActionPreference = $PrevEap
    if ($PipCode -ne 0) {
        throw "Falha ao instalar lz4/unicorn/capstone na .venv (exit $PipCode)"
    }

    $PrevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $Python -c "import lz4.block, unicorn, capstone" 2>$null
    $DepsCode = $LASTEXITCODE
    $ErrorActionPreference = $PrevEap
    if ($DepsCode -ne 0) {
        throw "A .venv ainda nao consegue importar lz4/unicorn/capstone."
    }
}

Write-Host "[scarlet-gen] Python: $Python"

if ($Clean -and (Test-Path -LiteralPath $OutDir)) {
    if ($Module -eq "all") {
        Write-Host "[scarlet-gen] Limpando $OutDir"
        Remove-Item -LiteralPath $OutDir -Recurse -Force
    }
    else {
        $ModuleOut = Join-Path $OutDir $Module
        if (Test-Path -LiteralPath $ModuleOut) {
            Write-Host "[scarlet-gen] Limpando somente $ModuleOut"
            Remove-Item -LiteralPath $ModuleOut -Recurse -Force
        }
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Jobs = @(
    @{
        Name = "sdk"
        Base = "0x7006000000"
        Extra = @(
            "--indirect-resume",
            "--hle", (Join-Path $RepoRoot "runtime\hle\hle_symbols.txt"),
            "--extra-entry", "0x7006323db4",
            "--extra-entry-range", "0x700640d240:0x700640d340"
        )
    },
    @{
        Name = "main"
        Base = "0x7000000000"
        Extra = @()
    },
    @{
        Name = "subsdk0"
        Base = "0x7008000000"
        Extra = @()
    }
)

foreach ($Job in $Jobs) {
    $Name = $Job.Name
    if ($Module -ne "all" -and $Name -ne $Module) {
        continue
    }
    $InputPath = Join-Path $ExeFs $Name
    $OutputPath = Join-Path $OutDir $Name
    New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null

    Write-Host ""
    Write-Host "[scarlet-gen] Gerando $Name ..." -ForegroundColor Cyan

    $Args = @(
        (Join-Path $RepoRoot "tools\gen.py"),
        $Name,
        $InputPath,
        $Job.Base,
        $OutputPath,
        "--simd", "c"
    ) + $Job.Extra

    & $Python @Args
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao gerar $Name (exit $LASTEXITCODE)"
    }
}

Write-Host ""
Write-Host "[scarlet-gen] Geracao ARM64 -> C concluida." -ForegroundColor Green

Get-ChildItem -LiteralPath $OutDir -Directory | ForEach-Object {
    $Files = @(Get-ChildItem -LiteralPath $_.FullName -Filter *.c -File)
    $Bytes = ($Files | Measure-Object -Property Length -Sum).Sum
    if ($null -eq $Bytes) { $Bytes = 0 }
    [PSCustomObject]@{
        Module = $_.Name
        CFiles = $Files.Count
        SizeMB = [Math]::Round($Bytes / 1MB, 1)
    }
} | Sort-Object Module | Format-Table -AutoSize

