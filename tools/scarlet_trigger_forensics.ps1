param(
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"
$build = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null
$flag = Join-Path $build "forensics.request"
Set-Content -Path $flag -Value "capture" -Encoding ascii
Write-Host "[scarlet-forensics] Pedido de captura enviado."
Write-Host "[scarlet-forensics] Volte para o jogo e aguarde alguns segundos."
