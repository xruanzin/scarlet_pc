param([string]$LogPath = "", [int]$RefreshMs = 500)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $RepoRoot "scarlet_build.log" }
if ($RefreshMs -lt 100) { $RefreshMs = 100 }

function Format-Duration([double]$Seconds) {
    if ($Seconds -lt 0) { return "--:--" }
    $ts = [TimeSpan]::FromSeconds([Math]::Round($Seconds))
    if ($ts.TotalHours -ge 1) { return "{0:00}:{1:00}:{2:00}" -f [int]$ts.TotalHours,$ts.Minutes,$ts.Seconds }
    return "{0:00}:{1:00}" -f $ts.Minutes,$ts.Seconds
}
$total = 0
foreach ($mod in @("main","sdk","subsdk0")) {
    $dir = Join-Path $RepoRoot ("generated\\" + $mod)
    if (Test-Path $dir) {
        $total += @(Get-ChildItem $dir -Filter ($mod + "_*.c") -File | Where-Object { $_.Name -notmatch "_table\\.c$" }).Count
    }
}
$start = Get-Date
$lastLength = -1L
Write-Host "[scarlet-progress] Monitorando $LogPath" -ForegroundColor Cyan
Write-Host "[scarlet-progress] Ctrl+C fecha somente este monitor." -ForegroundColor DarkGray

while ($true) {
    if (-not (Test-Path $LogPath -PathType Leaf)) {
        Write-Progress -Activity "Scarlet PC - Build" -Status "Aguardando scarlet_build.log..." -PercentComplete 0
        Start-Sleep -Milliseconds $RefreshMs
        continue
    }
    $item = Get-Item $LogPath
    if ($item.Length -eq $lastLength) { Start-Sleep -Milliseconds $RefreshMs; continue }
    $lastLength = $item.Length
    $lines = @(Get-Content $LogPath -ErrorAction SilentlyContinue)
    $game = @($lines | Where-Object { $_ -match "^\\[game-cc\\]\\s+(.+)$" })
    $count = $game.Count
    $current = "Preparando compilacao..."
    if ($game.Count -gt 0) {
        $m = [regex]::Match($game[-1], "^\\[game-cc\\]\\s+(.+)$")
        if ($m.Success) { $current = $m.Groups[1].Value }
    }
    $elapsed = ((Get-Date)-$start).TotalSeconds
    $percent = if ($total -gt 0) { [Math]::Min(99,[Math]::Floor($count*100.0/$total)) } else { 0 }
    $eta = "--:--"
    if ($count -gt 0 -and $total -gt $count) {
        $rate = $count/[Math]::Max($elapsed,0.001)
        if ($rate -gt 0) { $eta = Format-Duration (($total-$count)/$rate) }
    }
    if ($lines | Select-String -SimpleMatch "BUILD CONCLUIDO" -Quiet) {
        Write-Progress -Activity "Scarlet PC - Build" -Completed
        Write-Host "[##############################] 100%  BUILD CONCLUIDO" -ForegroundColor Green
        break
    }
    if ($lines | Select-String -Pattern "(error:|make: \\*\\*\\*|Build falhou)" -Quiet) {
        Write-Progress -Activity "Scarlet PC - Build" -Completed
        Write-Host "[ERRO] Build falhou. Veja scarlet_build.log." -ForegroundColor Red
        break
    }
    $totalText = if ($total -gt 0) { $total } else { "?" }
    $status = "{0}% | {1}/{2} | tempo {3} | ETA {4}" -f $percent,$count,$totalText,(Format-Duration $elapsed),$eta
    Write-Progress -Activity "Scarlet PC - Build" -Status $status -CurrentOperation $current -PercentComplete $percent
    Clear-Host
    Write-Host "Scarlet PC - Build" -ForegroundColor Cyan
    Write-Host ""
    $width=30; $filled=[Math]::Floor($width*$percent/100.0); $bar=("#"*$filled).PadRight($width,"-")
    Write-Host ("[{0}] {1,3}%" -f $bar,$percent)
    Write-Host ""
    Write-Host ("Arquivos: {0} / {1}" -f $count,$totalText)
    Write-Host ("Tempo:    {0}" -f (Format-Duration $elapsed))
    Write-Host ("ETA:      {0}" -f $eta)
    Write-Host ("Atual:    {0}" -f $current)
    Write-Host ""
    Write-Host "Ctrl+C fecha apenas o monitor; o build continua." -ForegroundColor DarkGray
    Start-Sleep -Milliseconds $RefreshMs
}