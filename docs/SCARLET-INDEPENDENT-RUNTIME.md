# Scarlet PC — independent runtime path

Scarlet PC is designed to run without requiring a separately installed emulator/runtime package.

The current independence milestone is: the repository builds its own Windows host, combines it with Scarlet's AOT modules, and produces `ScarletPC.exe`.

Eden was used as a technical reference for runtime architecture, build organization and integration ideas while shaping this path. The Pokémon Scarlet-specific AOT integration, packaging, diagnostics, compatibility work, port-specific functions and performance optimizations are implemented and maintained by **xruanzin** for Scarlet PC.

This is runtime/package independence. Third-party code provenance and licensing are documented separately in `THIRD_PARTY.md`.

## CPU path

The self-built host is AOT-first.

Scarlet's four recompiled modules are loaded natively and remain the primary execution path. Dynarmic/JIT is compiled into the Scarlet PC runtime only as a guarded fallback for code that is generated, modified or otherwise unavailable to the static AOT image at runtime.

The runner uses the non-strict fallback policy so a guarded AOT mismatch can hand execution to the internal fallback instead of terminating the process. When execution returns to covered code, the runtime can resume the static AOT path.

## Runtime baseline

Validated runtime revision:

`347fcf91d95cd39cb5c3db50f1b6d827ad699311`

The source is fetched into `third_party/scarlet-runtime-src` and treated as a third-party build input, not as original Scarlet PC authorship.

Technical architecture/build decisions for the independent host were informed by Eden as a reference. Exact third-party provenance is kept in `THIRD_PARTY.md`.

## Build

Run:

```powershell
.\tools\scarlet_independent_setup.ps1 -GamePath "$env:USERPROFILE\Downloads\Pokemon Scarlet\Pokemon Scarlet.nsp" -Jobs 1
```

Existing Scarlet AOT modules are reused when present.

The package is created at `dist\ScarletPC-Independent` and started with:

```powershell
.\tools\scarlet_independent_run.ps1
```

The final player-facing goal is a normal Scarlet PC flow: select a legally obtained game once, prepare the local game-derived AOT cache, then launch from `ScarletPC.exe` without a separate runtime installation or terminal workflow.


## Product-facing runtime layout

The independent package exposes Scarlet-owned names:

    ScarletPC.exe
    aot/scarlet-main.dll
    aot/scarlet-rtld.dll
    aot/scarlet-multimedia.dll
    aot/scarlet-sdk.dll
    user/config/scarlet.ini
    user/cache/scarlet/
    mods/
    licenses/third-party/

The game window title is fixed to **Pokémon Scarlet**. Player-facing runtime messages use the Scarlet categories `Scarlet.Runtime`, `Scarlet.AOT`, `Scarlet.Graphics` and `Scarlet.Game`.

The public runtime boundary is:

    ScarletPC.exe
          ↓
    Scarlet::GameRuntime
          ↓
    Nswitch::Runtime
          ↓
    AOT / Vulkan / HLE / dynamic fallback

The current executable is still a monolithic host internally. The planned `nswitch-runtime.dll`, `nswitch-video.dll` and `nswitch-audio.dll` split is intentionally deferred until those components have real stable ABI boundaries; the project does not ship fake placeholder DLLs.

The launcher vocabulary is defined in `config/scarlet-ui.json`: the normal UI says **Jogar**, **Desempenho**, **Qualidade visual**, **Controles** and **Idioma**, while container/runtime implementation terms stay out of the player-facing flow.
