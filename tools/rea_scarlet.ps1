param(
    [ValidateSet("setup","doctor","analyze-runtime","analyze-game","function","trace")]
    [string]$Action = "doctor",
    [ValidateSet("ghidra","hopper","ida")]
    [string]$Provider = "ghidra",
    [string]$Target = "",
    [string]$Query = "",
    [string]$Snapshot = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Require-Node {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) { throw "Node.js nao encontrado. REA exige Node.js 22.19+, 24.11+ ou 26+." }
    $v = & node --version
    Write-Host "[rea-scarlet] Node $v" -ForegroundColor DarkGray
}

function Invoke-Rea([string[]]$Args) {
    Require-Node
    Write-Host ("[rea-scarlet] npx -y rea-agents@latest " + ($Args -join " ")) -ForegroundColor Cyan
    & npx -y rea-agents@latest @Args
    if ($LASTEXITCODE -ne 0) { throw "REA terminou com exit code $LASTEXITCODE" }
}

switch ($Action) {
    "setup" {
        Invoke-Rea @("setup")
        break
    }
    "doctor" {
        Invoke-Rea @("doctor","--json")
        break
    }
    "analyze-runtime" {
        if (-not $Target) { $Target = Join-Path $RepoRoot "build\ScarletRecomp.exe" }
        if (-not (Test-Path -LiteralPath $Target -PathType Leaf)) { throw "Target ausente: $Target" }
        $args = @("analyze",(Resolve-Path -LiteralPath $Target).Path,"--provider",$Provider,"--json")
        if ($Snapshot) { $args += @("--snapshot",$Snapshot) }
        Invoke-Rea $args
        break
    }
    "analyze-game" {
        if (-not $Target) {
            $candidates = @(
                (Join-Path $RepoRoot "extracted\exefs\main"),
                "D:\Pokemon Scarlet\Extracted\Base\exefs\main"
            )
            $Target = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        }
        if (-not $Target -or -not (Test-Path -LiteralPath $Target -PathType Leaf)) {
            throw "main do Scarlet nao encontrado. Passe -Target <caminho para exefs\main>."
        }
        $args = @("analyze",(Resolve-Path -LiteralPath $Target).Path,"--provider",$Provider,"--json")
        if ($Snapshot) { $args += @("--snapshot",$Snapshot) }
        Invoke-Rea $args
        break
    }
    "function" {
        if (-not $Target) { throw "-Target e obrigatorio." }
        if (-not $Query) { throw "-Query deve ser nome ou endereco da funcao." }
        Invoke-Rea @("function",(Resolve-Path -LiteralPath $Target).Path,$Query,"--provider",$Provider,"--json")
        break
    }
    "trace" {
        if (-not $Target) { throw "-Target e obrigatorio." }
        if (-not $Query) { throw "-Query e obrigatorio." }
        Invoke-Rea @("trace",(Resolve-Path -LiteralPath $Target).Path,$Query,"--provider",$Provider,"--json")
        break
    }
}
