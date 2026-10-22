# Scarlet PC — AOT bootstrap

## Validated architecture

Scarlet PC now has a working Windows x86-64 AOT bootstrap using a small host executable plus four shared recompiled modules:

```text
ScarletPC.exe
recompiled_rtld.dll
recompiled_image.dll
recompiled_subsdk0.dll
recompiled_sdk.dll
```

The large game-derived code remains in the shared DLLs instead of being linked into a single oversized PE executable.

## Validation result

The current bootstrap was validated with Pokémon Scarlet title ID `0100A3D008C5C000` and suyu v14.1.0.

Observed in the validation run:

- all four `recompiled_*.dll` modules loaded successfully;
- ArmRecomp became active for the application process;
- image ABI 6 negotiated successfully;
- page-table fastmem negotiated successfully;
- guard-v2 negotiated successfully;
- FPX1 native floating point negotiated successfully;
- the game process started and created guest threads;
- Vulkan initialized on the host;
- Pokémon Scarlet entered the game and rendered normally;
- the recorded startup run reported zero recompilation gaps.

One observed package layout used approximately:

```text
ScarletPC.exe                 ~30 MB
recompiled_image.dll        ~1.2 GB
recompiled_rtld.dll         <1 MB
recompiled_sdk.dll          ~120 MB
recompiled_subsdk0.dll       ~66 MB
```

These sizes are diagnostic only and may change across builds.

## Why shared AOT modules

A previous single-file export produced a very large PE image and was not a good development shape for Scarlet PC.

Shared modules are preferred because they:

- keep the host executable small;
- isolate main / rtld / subsdk0 / sdk;
- make rebuilds and diagnostics easier;
- avoid relinking all translated game code into one executable;
- match the compatibility loader already present in suyu v14.1.0.

## Build from an existing Source export

The first bootstrap intentionally starts from a Source export already produced by the user's own suyu installation.

On a 16 GB machine, use one compiler job:

```powershell
.\tools\scarlet_aot_build.ps1 \
  -ExportRoot "D:\scarlet\Pokémon Scarlet - Hybrid AOT + JIT" \
  -Jobs 1
```

This collects the generated outputs into:

```text
build\aot\modules\
  recompiled_image.dll
  recompiled_rtld.dll
  recompiled_subsdk0.dll
  recompiled_sdk.dll
  scarlet-aot-manifest.json
```

## Package the runtime

```powershell
.\tools\scarlet_aot_package.ps1 \
  -SuyuDir "$env:USERPROFILE\Downloads\scarlet_pc\suyu-windows-x86_64"
```

The local package is created at:

```text
dist\ScarletPC\
```

## One-command setup after the Source export exists

```powershell
.\tools\scarlet_aot_setup.ps1 \
  -ExportRoot "D:\scarlet\Pokémon Scarlet - Hybrid AOT + JIT" \
  -SuyuDir "$env:USERPROFILE\Downloads\scarlet_pc\suyu-windows-x86_64" \
  -GamePath "$env:USERPROFILE\Downloads\Pokemon Scarlet\Pokemon Scarlet.nsp" \
  -Jobs 1
```

Then:

```powershell
.\tools\scarlet_aot_run.ps1
```

## Repository policy

The repository stores the tooling and Scarlet-specific integration, not the game.

Never commit:

- NSP/XCI/NCA/NSO game files;
- prod.keys/title.keys;
- firmware;
- generated `recompiled_*.dll` files;
- AOT caches or intermediate compiler output.

The next bootstrap milestone is automating Source-export generation itself so the user no longer needs to create that 4 GB export manually.
