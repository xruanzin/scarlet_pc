# Third-party foundations

Scarlet PC is a Pokémon Scarlet-specific Windows recompilation project created and maintained by **xruanzin**.

The project uses and modifies third-party open-source components where appropriate. Those components retain their own copyright notices and licenses.

## Scarlet PC original work

The Pokémon Scarlet-specific implementation in this repository is developed and maintained by **xruanzin**.

This includes the port-specific functionality and integration required to make Pokémon Scarlet run through the Scarlet PC pipeline, including:

- Pokémon Scarlet-specific AOT integration and module handling;
- Scarlet-specific runtime integration and compatibility work;
- build, packaging, setup and launch tooling created for this port;
- diagnostics and instrumentation added for this project;
- port-specific fixes and runtime patches;
- performance profiling and optimizations developed for Scarlet PC;
- CPU/AOT execution-path tuning and fallback integration;
- Windows packaging and the standalone Scarlet PC runtime workflow.

These Scarlet-specific functions, fixes and optimizations are Scarlet PC project work and are not claimed as part of the unmodified upstream runtime.

## suyu

Scarlet PC's runtime/recompiler foundation incorporates modified GPL-licensed work derived from **suyu**:

https://github.com/suyu-emu/suyu-main

The validated upstream baseline is suyu v14.1.0 / revision `347fcf91d95cd39cb5c3db50f1b6d827ad699311`.

suyu is licensed under GNU GPL v3 and retains its own upstream lineage and notices.

Scarlet PC does not claim authorship of the original suyu runtime/recompiler. Credit to suyu applies to the third-party foundation from which portions of the runtime were derived.

The Pokémon Scarlet-specific port functionality, integration, compatibility changes, tooling and performance optimizations described above were implemented and maintained for Scarlet PC by **xruanzin**.

## Game content

Scarlet PC does not distribute Pokémon Scarlet, Nintendo Switch firmware, console keys, or generated game-derived binaries. Users must provide their own legally obtained game files and required console data.


## Graphics integration dependencies

### AMD FidelityFX SDK
- Source: GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- Pinned revision used by Scarlet PC: `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- License: MIT
- Use in Scarlet PC: Vulkan FidelityFX temporal upscaling and FSR 3 frame-generation runtime.
- The packaged `amd_fidelityfx_vk.dll`, when present, remains AMD software under its
  accompanying license notice.

### NVIDIA DLSS / NGX SDK
- Source: NVIDIA/DLSS
- Pinned revision used by the optional Scarlet bridge:
  `9981c6350dc58c0ce21f024fd65e31e851c31689`
- License: NVIDIA SDK license included with the fetched SDK.
- Use in Scarlet PC: optional NVIDIA NGX DLSS Super Resolution sidecar.
- `ScarletDlssBridge.dll` is project-owned integration code; `nvngx_dlss.dll` remains
  NVIDIA software and is packaged only with its corresponding notice.

### GoW3 NVIDIA DLSS bridge design
- Reference: DestroxIam/gow3_pc-nvidia, `gpu/dlss_bridge`
- License for that bridge: MIT
- Scarlet PC uses the same isolated-sidecar architecture and retains bridge attribution.
  It does not reuse GoW3's registered NGX project identifier or copy its GPL renderer.

### FireBurn FSR-Vulkan
- Source: FireBurn/FSR-Vulkan
- Revision referenced by the GoW3 NVIDIA fork:
  `c64f093404125e960813f95f1996c14780f0dd69`
- License: MIT
- Intended use: native Vulkan FSR 3.1.x provider and experimental source-v07 FSR 4 provider.
- The experimental FSR 4 v07 provider is not AMD's signed FSR 4.1.1 implementation and
  requires its own compatible model/shader asset bundle.
