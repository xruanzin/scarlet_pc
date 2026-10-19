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
$env:LARECOMP_VK_ALLOW_13 = "1" # for the Vega 7 driver reporting Vulkan 1.3
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_run.ps1 -Backend Vulkan
```

Vulkan remains the default; selection is not persisted. PATH, ICD selectors,
Mesa version override and GPU index changes are limited to the launch and
restored in finally, including exception paths. No driver is registered.
The existing AMD Vulkan driver remains available after deleting the portable
Mesa directory. The game EXE and save locations are the same.

The runtime checks the actual selected driver ID: DirectX requires hardware
Mesa Dozen; explicit Vulkan excludes Dozen. No API-version override or feature
spoofing is used. Native Vulkan now uses 1.3 as requested, independently of loader 1.4 support.
DirectX still negotiates 1.2. The Dozen path negotiates Vulkan 1.2 plus
KHR/EXT equivalents and checks required features; a combination missing them
will fail initialization with the actual extension or feature name.
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

## Driver selection diagnostics

Use an ordinary, non-administrator PowerShell. The Khronos loader ignores
VK_DRIVER_FILES / VK_ICD_FILENAMES with elevated privileges:
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md
The launcher now detects elevation before a DirectX attempt and explains the
restriction (or visibly falls back). It does not bypass the loader policy.

DirectX attempts enable loader warnings/errors/driver diagnostics. The runtime
logs the requested ICD and enumerated driver IDs/names, and distinguishes a
missing eligible Dozen device from the absence of native Vulkan 1.3 support.
These changes diagnose loading; they do not establish that a particular Dozen
build exposes all features needed for Scarlet. The reported AMD-only attempt
is consistent with a failed/ignored ICD override; elevation is one possible
cause, not established by that earlier log alone.

## Vulkan 1.2 negotiation

The DirectX path now requests Vulkan 1.2 at instance creation and accepts a
hardware Dozen 1.2 device. It requires and enables:

- VK_KHR_dynamic_rendering
- VK_KHR_synchronization2
- VK_EXT_extended_dynamic_state
- VK_EXT_extended_dynamic_state2

It queries and validates their feature bits, timeline semaphores, shaderInt64,
independentBlend and BC texture support before device creation. It does not
request a Vulkan13Features structure in this path. Optional 8/16-bit/scalar
features are enabled only when reported; optional border/divisor/fault
extensions are disabled when their needed feature bits are unavailable.

Fourteen promoted commands are resolved using KHR/EXT entry points on 1.2 and
core names on native 1.3. Structures/constants retain their standard aliased
values. References:
https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_dynamic_rendering.html
https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_synchronization2.html

Shaderc targets Vulkan 1.2 for Dozen (including compute specialization constants),
VMA and ImGui receive the negotiated API, and the SPIR-V target cache is separated.
Dozen pipeline caches use cache/vk_pipelines_dzn12.bin, preserving native Vulkan's
cache/vk_pipelines.bin. Vulkan remains selectable and the startup fallback remains.

Validation includes the actual production device-creation function against a
mock ICD: 1.2 extension and 1.3 core paths, missing extension/feature/command,
and optional extensions without usable features. Real shaderc compiles compute
for Vulkan 1.2, with spirv-val validation and a two-target disk-cache regression.
Native vertex invariance and transform-feedback shader tests pass. Modified
Vulkan C/C++ units compile. No Windows/Dozen GPU game validation is available;
this removes the unconditional 1.3 version gate, not all possible driver limits.

The native policy was restored to Vulkan 1.3 by commit dccd831. DirectX/Dozen
continues to negotiate 1.2 separately. The native API policy tests now match that
choice even when the loader advertises 1.4.

The hair investigation uses native Vulkan 1.3 by default in scarlet_run.ps1.
See SCARLET-HAIR-CAPTURE.md for the new -RenderDoc frame-capture launch option.
Standalone EXE invocation also uses Vulkan 1.3; LARECOMP_VK_ALLOW_13 is no
longer required to select it.
