param(
    [string]$GamePath,
    [string]$UpdatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot -Parent
$package = Join-Path $repoRoot 'dist\ScarletPC-Independent'
$exe = Join-Path $package 'ScarletPC.exe'

if (-not (Test-Path $exe -PathType Leaf)) {
    throw 'Independent package not found. Run tools\scarlet_independent_setup.ps1 first.'
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    $cfgPath = Join-Path $repoRoot 'build\scarlet-independent-local.json'
    if (Test-Path $cfgPath) {
        $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
        $GamePath = $cfg.game_path
        if ([string]::IsNullOrWhiteSpace($UpdatePath) -and $cfg.PSObject.Properties.Name -contains 'update_path') {
            $UpdatePath = [string]$cfg.update_path
        }
    }
}

if ([string]::IsNullOrWhiteSpace($GamePath)) {
    throw 'Pass -GamePath or run scarlet_independent_setup.ps1 first.'
}

$game = (Resolve-Path $GamePath).Path
$update = $null
if (-not [string]::IsNullOrWhiteSpace($UpdatePath)) {
    $update = (Resolve-Path $UpdatePath).Path
}
else {
    $autoUpdate = Join-Path (Split-Path $game -Parent) 'Pokemon Scarlet v4.0.0.nsp'
    if (Test-Path $autoUpdate -PathType Leaf) {
        $update = (Resolve-Path $autoUpdate).Path
    }
}

$prodKeys = Join-Path $package 'user\keys\prod.keys'
if (-not (Test-Path $prodKeys -PathType Leaf)) {
    throw 'Scarlet PC cannot decrypt the game because user\keys\prod.keys is missing. Re-run scarlet_independent_setup.ps1 with -KeysPath pointing to your legally obtained prod.keys or keys folder.'
}

$provenancePath = Join-Path $repoRoot 'licenses\third-party\runtime-provenance.json'
$legacyStrictEnv = $null
if (Test-Path $provenancePath -PathType Leaf) {
    $provenance = Get-Content $provenancePath -Raw | ConvertFrom-Json
    $legacyStrictEnv = [string]$provenance.legacy.strict_env
}

Push-Location $package
try {
    $env:NSWITCH_AOT_FALLBACK = '1'
    if (-not [string]::IsNullOrWhiteSpace($legacyStrictEnv)) {
        Set-Item -Path ("Env:" + $legacyStrictEnv) -Value '0'
    }
    # ScarletPC.exe is a Windows GUI executable. Invoking it with '&' can return
    # before PowerShell initializes $LASTEXITCODE, which is an error under StrictMode.
    # Start and wait on the actual Process object instead so its exit status is reliable.
    $launchArgs = @()
    if ($update) {
        Write-Host "[Scarlet PC] Base:   $game" -ForegroundColor DarkGray
        Write-Host "[Scarlet PC] Update: $update" -ForegroundColor Green
        $launchArgs = @('--content-base', ('"{0}"' -f $game),
                        '--content-update', ('"{0}"' -f $update))
    }
    else {
        Write-Warning '[Scarlet PC] No update NSP found; launching base game only.'
        $launchArgs = @('-g', ('"{0}"' -f $game))
    }

    $process = Start-Process -FilePath $exe -ArgumentList $launchArgs `
        -PassThru -Wait -NoNewWindow
    $code = $process.ExitCode
    if ($code -ne 0) {
        throw "ScarletPC.exe exited with code $code"
    }
}
finally {
    Remove-Item Env:NSWITCH_AOT_FALLBACK -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($legacyStrictEnv)) {
        Remove-Item -Path ("Env:" + $legacyStrictEnv) -ErrorAction SilentlyContinue
    }
    Pop-Location
}
