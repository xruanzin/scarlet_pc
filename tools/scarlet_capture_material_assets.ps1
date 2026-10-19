param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [ValidateSet("Vulkan")]
    [string]$Backend = "Vulkan"
)

$ErrorActionPreference = "Stop"

# Asset captures must reflect the guest material as-is. Do not inherit graphics
# A/B experiments from an earlier PowerShell session.
$vars = @(
    "LARECOMP_BC7_SWAP_RB",
    "LARECOMP_BC7_LINEAR",
    "LARECOMP_HAIR_ISOTROPIC",
    "LARECOMP_NEUTRAL_HAIR_TINT",
    "LARECOMP_NEUTRAL_HAIR_OFF",
    "LARECOMP_LOG_MATERIAL_DRAWS",
    "LARECOMP_LOG_MATERIAL_VIEWS",
    "LARECOMP_LUT_IDENTITY",
    "LARECOMP_VK_RAW_SRGB_ATTACH",
    "LARECOMP_VK_SRGB_ATTACH"
)
foreach ($name in $vars) {
    if (Test-Path "Env:$name") {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        Write-Host "[asset-capture] Cleared diagnostic: $name" -ForegroundColor DarkYellow
    }
}

Write-Host ""
Write-Host "=== Scarlet material asset capture ===" -ForegroundColor Cyan
Write-Host "1. Entre na cena onde o cabelo esta azul/roxo." -ForegroundColor White
Write-Host "2. Quando o cabelo bugado estiver bem visivel, pressione F9 UMA vez." -ForegroundColor White
Write-Host "3. Espere 3-5 segundos para os dumps terminarem." -ForegroundColor White
Write-Host "4. Feche o jogo normalmente." -ForegroundColor White
Write-Host ""
Write-Host "O capture inclui: draw list, shaders GLSL, MRT/pass images e fontes brutas das texturas ligadas ao frame." -ForegroundColor Gray
Write-Host ""

$run = Join-Path $PSScriptRoot "scarlet_run.ps1"
$collect = Join-Path $PSScriptRoot "scarlet_collect_forensics.ps1"

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $run -Backend $Backend
if ($LASTEXITCODE -ne 0) {
    throw "Scarlet terminou com exit code $LASTEXITCODE; capture nao coletado."
}

Write-Host "[asset-capture] Jogo fechado; empacotando o ultimo frame F9..." -ForegroundColor Cyan
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $collect -Root $Root
if ($LASTEXITCODE -ne 0) {
    throw "Falha ao coletar o capture forense."
}
