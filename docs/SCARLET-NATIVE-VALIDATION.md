# Native runtime validation — 2026-10-27

This work extends `native/scarlet-runtime-migration`; it is not evidence of a
completed gameplay migration. The initial checkout was clean at `0fd5691`.
Concurrent migration changes through `041207f` were preserved. No game DLL was
regenerated and no existing graphics, save, input or audio implementation was
removed.

## Executable path already present in the migration branch

`ScarletPC.exe` selects native by default, loads the four ABI-6 ArmRecomp DLLs,
loads/relocates NSOs with `runtime/loader.c`, binds main/subsdk0/sdk, and enters
`scarlet_aot_guest_call`. The DLL's `run_slice` handles module-local chaining;
module transitions return to the native bridge. CPU state is transferred at
SVC/HLE boundaries to the native `runtime/kernel.c` and HLE/service code.
`rtld` is loaded but **not executed**: the native NSO loader owns relocation and
bootstrap. This differs from executing the original rtld entry.

This target links the repository's native NVN/Vulkan implementation, including
its existing render thread, caches, keyboard overlay and FSR providers; it does
not link suyu's CpuManager, PhysicalCore, KernelCore, KScheduler or video_core.
That is a source/build dependency statement, not verified parity with the
legacy renderer or verified gameplay boot.

## Changes in this validation branch

- Failed DLL load/ABI negotiation releases every image acquired by the attempt,
  clears function pointers, and permits a clean retry.
- An explicit `SCARLET_AOT_DIR` cannot silently fill missing DLLs from a different
  bundle. Rebinding invalidates previous readiness and checks index alignment
  and bounds before indexing.
- Native launcher readiness accepts the existing AOT bundle without lasgame.
  Setup no longer invokes the LAS generator in native mode. It reports missing
  AOT separately, recognizes the selected extracted root or package mount paths,
  and accepts either a RomFS image or directory.
- The Windows runner quotes paths containing spaces and restores the previous
  HLE profiler setting after a run, including a failed launch.
- Fixed an existing Python syntax error (`0xFFFull`) in PLT decoding that blocked
  importing the generator in existing tests. This does not run the generator or
  rebuild any AOT image.
- CI executes synthetic native tests and retains the migration branch's full
  Windows executable link, instead of treating object compilation as completion.

## Validation performed

`python tools/test_native_aot_loader.py` compiles the production loader and
execution bridge with synthetic DLL exports. It exercises all four images,
ABI/features, load failure at each image, rollback/retry, missing exports,
invalid index/rebind, cross-module execution, SVC/HLE returns, GPR, SIMD, FP state
and TLS. Undefined-behavior sanitization is enabled. DLL API calls are test
doubles; these are not real game DLLs.

`python tools/test_native_setup.py` compiles real setup code against synthetic
NPDM data and tests native versus LAS selection, no native regeneration,
missing-AOT errors, a root containing spaces and mounted paths.

Existing `test_thread_tls.py`, `test_condition_publication.py` and
`test_targeted_sync_wake.py` passed locally. The Linux kernel harness used a
minimal SDL CPU-count declaration; no SDL window/GPU was exercised.

## Reference code consulted

| Reference snapshot | Code / conclusion |
| --- | --- |
| LASRecomp `a4b53a3` | `runtime/dispatch.c`, `tools/gen.py`: direct-mapped lookup and tail dispatch; generated direct calls/local gotos belong to the generated ABI, not a host compiler flag. |
| letsgo-recomp `4145712` | `runtime/kernel.c`, `docs/HISTORICO.md` items 57–58, generator and render-thread sources: bounded spin waits for ownership handoff, not indiscriminate busy waiting; dedicated vertex-copy strategy must distinguish discrete GPU from UMA. Existing Scarlet code already carries related mechanisms. |
| PokeZ-ARecomp `a47f1e1` | dispatch/generator: direct transfers and local labels; no new generator optimization was copied into the preserved ArmRecomp DLLs. |
| ssbu-recomp `fea28c1` | `runtime/nvn_front.h`, `nvn_vk_draw.c`, `nvn_vk_prewarm.h`: explicit render-thread ownership and prewarm worker diagnostics. No literal SSBU implementation was incorporated in this change. |

This is a scoped comparison, not an exhaustive audit of all four projects.

## Remaining blockers and fallbacks

- Real Windows AOT DLLs and game assets were unavailable in this environment.
  Logs/menu/load/gameplay, actual imports and SVC coverage cannot be certified.
- Updates require BKTR/patch-overlay semantics. The concurrently added runner
  explicitly routes `-UpdatePath` through the legacy executable; native update
  execution has not been implemented here.
- A compatible LAS library, if present, remains an optional fallback for missing
  AOT targets/instructions. `SCARLET_NATIVE_LAS_FALLBACK=0` makes such gaps fatal.
  This fallback is different from running the suyu-based legacy executable.
- Existing ABI validation does not establish matching NSO build IDs. The bundle
  must match the game version; synthetic tests do not prove binary compatibility.
- Fullscreen, saves, audio, controllers, graphics fixes, FSR temporal inputs,
  FSR4 and DLSS parity require real-game validation. Presence of provider source
  alone does not establish those features work in this native path.
- No FPS or frame-time comparison was measured. The reported legacy baseline
  remains 17–20 FPS in heavy areas and 27–28 FPS in light areas. There is no
  measured native result and no measured new bottleneck ranking.

## Windows commands (existing checkout)

Inspect `git status --short --branch` first. Do not replace local changes or
switch branches over conflicting uncommitted work. Once on
`native/aot-launcher-validation` with your changes preserved:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_build.ps1 -Jobs 4 -AotDir .\dist\ScarletPC-Independent\aot
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime native -GamePath "D:\Pokemon Scarlet\Extracted\Base" -Profile
```

For a base package, use `-GamePath "D:\Pokemon Scarlet\Pokemon Scarlet.nsp"`;
the existing mount helper requires the user's keys and Fuse-Nx dependencies.
For the known legacy update path:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime legacy -GamePath "D:\Pokemon Scarlet\Pokemon Scarlet.nsp" -UpdatePath "D:\Pokemon Scarlet\update.nsp"
```

Build requires the existing MSYS2/UCRT64 Clang/make dependencies. Host rebuild
reuses AOT. The native profiler is opt-in via `-Profile`; it writes the frame CSV
to `build/native/profile/frames.csv` and diagnostic timing to the runtime log.
