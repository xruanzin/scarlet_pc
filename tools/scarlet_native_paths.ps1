# Single source of truth for native Scarlet build/artifact paths.
# The user's Windows installation keeps its checkout on C:, but all new
# objects, EXE, AOT staging, and runtime data must be written to D:.
function Resolve-ScarletNativeBuildRoot {
    param([string]$BuildRoot)
    $requested = $BuildRoot
    if ([string]::IsNullOrWhiteSpace($requested)) {
        $requested = $env:SCARLET_NATIVE_BUILD_ROOT
    }
    if ([string]::IsNullOrWhiteSpace($requested)) {
        if ($env:OS -eq 'Windows_NT') {
            if (-not (Test-Path -LiteralPath 'D:\' -PathType Container)) {
                throw 'Unidade D: indisponivel. Defina -BuildRoot em outra unidade com espaco; o build nao voltara automaticamente para C:.'
            }
            $requested = 'D:\ScarletNativeCache\native-build'
        } else {
            # Non-Windows uses the existing source-tree default.
            $requested = Join-Path (Split-Path $PSScriptRoot -Parent) 'build/native'
        }
    }
    if (-not [IO.Path]::IsPathRooted($requested)) {
        throw "O destino do build precisa ser absoluto: $requested"
    }
    return [IO.Path]::GetFullPath($requested)
}
