# Native base + update preparation

The native runner now accepts GamePath + UpdatePath + KeysPath, prepares a cached
patched ExeFS/RomFS, and launches build/native/ScarletPC.exe. It does not start
suyu for this path. DLC still selects legacy. Explicit Runtime legacy is unchanged.

This is automatic disk preparation, **not** an on-demand BKTR mount. First use
extracts the package containers, writes a raw base RomFS section, and uses
hactool --baseromfs to apply BKTR to the update's section 1. The non-raw output
is the final RomFS image; update ExeFS is used without mixing base modules.
Inputs and AOT DLLs are untouched. The cache may require tens of GB of temporary
free space (both containers, raw base, patched image) and substantial first-run
time. Private work directories are removed on completion/failure. A ready marker
is published only after NPDM, NSO and RomFS validation. Cache identity includes
both input paths, lengths and modification times; it is not a full content hash.
A per-cache lock rejects simultaneous preparation. Existing incomplete caches
are preserved and rejected.

Optional AotManifest validates all three NSO Build IDs against the supplied
aot_manifest.json, including on cache hits. The caller must supply the manifest
belonging to the DLL bundle; ABI/guest guards remain enabled.

Uses the existing official hactool 1.4.0 download convention or an explicit
HactoolPath/SCARLET_HACTOOL. Common RSA2048/SHA256 tickets are supported;
personalized/other tickets fail clearly. Keys are supplied locally. Tool argv
and key dumps are not written to preparation logs. No key or game download occurs.
The layout currently targets Scarlet Program NCA ExeFS and RomFS section 1.

Synthetic PowerShell tests exercise command ordering, raw base patch input,
published cache validation/reuse and failure isolation; runner tests check that
updates remain native and mount variables are restored. Real package decryption,
patch output and gameplay still require Windows testing with the user's assets.

Example (PowerShell, after fetching the branch):

```powershell
$env:Path = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:Path"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime native -GamePath "C:\Users\ruana\Downloads\Pokemon Scarlet - Pokemon Scarlet.nsp" -UpdatePath "D:\Pokemon Scarlet - Pokemon Scarlet v4.0.0.nsp" -KeysPath "C:\Users\ruana\Downloads\scarlet_pc\dist\ScarletPC-Independent\user\keys\prod.keys" -AotManifest "D:\scarlet\Pokémon Scarlet - Hybrid AOT + JIT\aot_cache\aot_manifest.json" -NoLauncher
```
