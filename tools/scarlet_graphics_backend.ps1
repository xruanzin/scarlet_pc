# Portable backend selection. No registry/system driver installation.
function Get-ScarletDirectXDriver {
    param([Parameter(Mandatory=$true)][string]$Directory)
    $Root = (Resolve-Path -LiteralPath $Directory -ErrorAction Stop).Path
    $Candidates = @(Get-ChildItem -LiteralPath $Root -Filter 'dzn_icd.x86_64.json' -File -Recurse)
    if ($Candidates.Count -ne 1) { throw 'Esperado exatamente um dzn_icd.x86_64.json (Mesa x64) na pasta DirectX.' }
    $Manifest = $Candidates[0].FullName
    $Data = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
    $Library = [string]$Data.ICD.library_path
    if (-not $Library) { throw 'Manifesto Dozen sem ICD.library_path.' }
    if (-not [IO.Path]::IsPathRooted($Library)) { $Library = Join-Path $Candidates[0].Directory.FullName $Library }
    $Library = (Resolve-Path -LiteralPath $Library -ErrorAction Stop).Path
    if ([IO.Path]::GetFileName($Library) -ne 'vulkan_dzn.dll') { throw 'O manifesto nao aponta para vulkan_dzn.dll.' }
    # Reject a 32-bit DLL before the loader tries to load it.
    $Stream = [IO.File]::OpenRead($Library)
    try {
        $Reader = New-Object IO.BinaryReader($Stream)
        if ($Reader.ReadUInt16() -ne 0x5a4d) { throw 'DLL Dozen sem cabecalho PE.' }
        $Stream.Position = 0x3c; $Offset = $Reader.ReadUInt32()
        $Stream.Position = $Offset
        if ($Reader.ReadUInt32() -ne 0x4550 -or $Reader.ReadUInt16() -ne 0x8664) { throw 'DLL Dozen precisa ser Windows x64.' }
    } finally { $Stream.Dispose() }
    return @{ Manifest = $Manifest; Directory = (Split-Path -Parent $Library) }
}

function Invoke-ScarletGraphicsBackend {
    param(
        [ValidateSet('Vulkan','DirectX')][string]$Backend = 'Vulkan',
        [string]$DirectXDirectory,
        [Parameter(Mandatory=$true)][string]$Log,
        [switch]$NoFallback,
        [Parameter(Mandatory=$true)][scriptblock]$Run
    )
    $Names = @('PATH','VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES',
        'VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE','MESA_VK_VERSION_OVERRIDE',
        'LARECOMP_GRAPHICS_BACKEND','LARECOMP_GPU')
    $Previous = @{}
    foreach ($Name in $Names) { $Previous[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process') }
    try {
        $Driver = $null
        if ($Backend -eq 'DirectX') {
            try { $Driver = Get-ScarletDirectXDriver -Directory $DirectXDirectory }
            catch {
                if ($NoFallback) { throw }
                Write-Host "[scarlet-run] DirectX indisponivel: $_ Voltando ao Vulkan." -ForegroundColor Yellow
                "[graphics-backend] DirectX preflight failed: $_" | Set-Content -LiteralPath "$Log.directx.failed.log"
                $Backend = 'Vulkan'
            }
        }
        foreach ($Name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','VK_ADD_DRIVER_FILES','VK_LOADER_DRIVERS_SELECT','VK_LOADER_DRIVERS_DISABLE','MESA_VK_VERSION_OVERRIDE')) {
            [Environment]::SetEnvironmentVariable($Name, $null, 'Process')
        }
        if ($Backend -eq 'DirectX') {
            $env:VK_DRIVER_FILES = $Driver.Manifest
            $env:VK_ICD_FILENAMES = $Driver.Manifest
            $env:PATH = $Driver.Directory + [IO.Path]::PathSeparator + $Previous['PATH']
            Remove-Item Env:LARECOMP_GPU -ErrorAction SilentlyContinue
            $env:LARECOMP_GRAPHICS_BACKEND = 'directx-dozen'
            Write-Host '[scarlet-run] DirectX 12 experimental via Mesa Dozen (traducao de Vulkan).'
        } else {
            $env:LARECOMP_GRAPHICS_BACKEND = 'vulkan'
            Write-Host '[scarlet-run] Vulkan nativo.'
        }
        $Code = [int](& $Run $Log)
        $Ready = (Test-Path -LiteralPath $Log) -and [bool](Select-String -LiteralPath $Log -Pattern '^\[graphics-backend\] ready=' -Quiet)
        # Retry only a failed initialization, never a crash after the renderer started.
        if ($Backend -eq 'DirectX' -and $Code -ne 0 -and -not $Ready -and -not $NoFallback) {
            if (Test-Path -LiteralPath $Log) { Copy-Item -LiteralPath $Log -Destination "$Log.directx.failed.log" -Force }
            Write-Host '[scarlet-run] DirectX falhou na inicializacao; reiniciando uma vez em Vulkan.' -ForegroundColor Yellow
            foreach ($Name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES')) { Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue }
            $env:PATH = $Previous['PATH']
            [Environment]::SetEnvironmentVariable('LARECOMP_GPU',$Previous['LARECOMP_GPU'],'Process')
            $env:LARECOMP_GRAPHICS_BACKEND = 'vulkan'
            $Code = [int](& $Run $Log)
        }
        return $Code
    } finally {
        foreach ($Name in $Names) { [Environment]::SetEnvironmentVariable($Name,$Previous[$Name],'Process') }
    }
}
