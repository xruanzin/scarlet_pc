# Scarlet PC — self-built runtime path

Scarlet PC must not require a separately installed suyu package.

The first independence milestone is: the repository fetches one pinned GPL runtime source revision, builds its own Windows host, combines it with Scarlet's AOT modules, and produces `ScarletPC.exe`.

This is runtime/package independence. The inherited runtime foundation remains GPL-derived work and keeps its provenance.

## CPU path

The self-built host is configured with `SUYU_NO_JIT=ON`, so Dynarmic/JIT is not part of this host. The validated Scarlet run already showed no static-to-JIT transitions and no unimplemented opcode transitions in the observed execution.

The runner also sets `SUYU_RECOMP_STRICT=1`, so an uncovered AOT address fails explicitly instead of silently using a fallback.

## Pinned runtime source

`347fcf91d95cd39cb5c3db50f1b6d827ad699311`

The source is fetched into `third_party/scarlet-runtime-src` and treated as a build input, not as original Scarlet PC authorship.

## Build

Run:

```powershell
.\tools\scarlet_independent_setup.ps1 -ExportRoot "D:\scarlet\Pokémon Scarlet - Hybrid AOT + JIT" -GamePath "$env:USERPROFILE\Downloads\Pokemon Scarlet\Pokemon Scarlet.nsp" -Jobs 1
```

There is no `-SuyuDir` argument.

The package is created at `dist\ScarletPC-Independent` and started with:

```powershell
.\tools\scarlet_independent_run.ps1
```

After validating this host, the next milestone is moving Source-export generation into Scarlet PC too, so the user no longer supplies `ExportRoot` manually.
