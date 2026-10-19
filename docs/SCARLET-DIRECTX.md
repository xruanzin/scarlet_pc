# Experimental DirectX 12 through Mesa Dozen

This is Vulkan translated to Direct3D 12 by Mesa Dozen, **not a native D3D12
renderer**. The current shader translator, Vulkan executor and Vulkan loader
remain in use. It is an alternative driver path for testing; graphics fixes,
performance and successful game boot on Dozen are not guaranteed.

Upstream package documentation: https://github.com/pal1000/mesa-dist-win/blob/master/readme.md
Dozen's ICD is `dzn_icd.x86_64.json` / `vulkan_dzn.dll`; its dependencies,
including DXIL, must remain together. Do not copy arbitrary DLLs into build/.

## Setup and launch (Windows PowerShell)

```powershell
cd "C:\Users\ruana\Downloads\scarlet_pc"
git pull --ff-only origin scarlet-bootstrap
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_build_windows.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_setup_directx.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_run.ps1 -Backend DirectX
```

Setup downloads the pinned Mesa 26.2.4 MSVC archive, verifies its SHA-256,
extracts with Windows tar.exe, validates the x64 Dozen manifest/DLL and keeps
all package files under tools/directx/mesa. It needs no administrator access,
registry changes or replacement of the AMD driver. Existing installations are
validated and reused. A supplied `-Archive` must match the same pinned checksum.
If tar.exe cannot extract 7z, extract the same publisher package with 7-Zip and
pass the extracted directory to `scarlet_run.ps1 -Backend DirectX -DirectXDirectory PATH`.

Return to the original native Vulkan driver:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_run.ps1 -Backend Vulkan
```

Vulkan remains the default; selection is not persisted. PATH, ICD selectors,
Mesa version override and GPU index changes are limited to the launch and
restored in finally, including exception paths. No driver is registered.
The existing AMD Vulkan driver remains available after deleting the portable
Mesa directory. The game EXE and save locations are the same.

The runtime checks the actual selected driver ID: DirectX requires hardware
Mesa Dozen; explicit Vulkan excludes Dozen. No API-version override or feature
spoofing is used. The port requires Vulkan 1.3 and additional device features;
a Dozen build/hardware combination that lacks them will fail initialization.
That is a limitation of this integration, not proof of working D3D12 gameplay.

## Failure handling

The launcher retries once with native Vulkan only if DirectX exits with an error
before `[graphics-backend] ready=directx-dozen`. It preserves the first log as
`scarlet_boot.log.directx.failed.log`; the main log describes the Vulkan attempt.
Missing/invalid packages also fall back visibly. `-NoBackendFallback` instead
reports the DirectX failure without retrying. Crashes after initialization are
never silently restarted. A zero exit code without renderer initialization is
not treated as successful DirectX rendering and does not trigger a retry.

Validation: PowerShell parser checks; synthetic x64 driver fixture tests for
selection, missing package, strict mode, one initialization retry, no retry after
ready, exception restoration and clearing inherited ICD selectors on Vulkan.
The modified device unit compiles. Windows Dozen/AMD GPU execution is untested.
