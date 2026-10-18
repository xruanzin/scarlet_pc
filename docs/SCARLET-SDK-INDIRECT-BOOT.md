# Scarlet: SDK boot stops before any game image

## Evidence from the supplied logs

The failing boot loads main, sdk and subsdk0, reaches the loader's SDK
constructor callback, then stops with:

```text
[boot] sdk DT_INIT status=1 @ 7006cea0d0
FATAL: no function at 700657c59c (lr=700657c41c vfprintf+6b4)
```

There is no `nvnBootstrapLoader` graphics milestone before this failure. The
log establishes a missing dispatch entry during CPU-side SDK initialization;
it does not establish a Vulkan, shader, resolution or GPU-driver failure.
The nearest exported symbol is `vfprintf`, but that symbol's reported size
does not cover the entire internal formatting implementation. The failed
instruction is not disassembled in the supplied auto-inspection, so the exact
original indirect-branch sequence still needs the user's extracted SDK.

The two Citron attachments are byte-for-byte identical. They boot version
1.0.0 and identify these ExeFS Build IDs:

| Module | Build ID |
| --- | --- |
| main | `CA44E7DD62E61A2A4E8889B3319AE173375770FD` |
| sdk | `3644A6760469451CABC99F00C540ACC2A64AEE41` |
| subsdk0 | `D0F13015E5BADF2243D60A2BC9B978DDDBA80608` |

They match the Scarlet baseline and the SDK ID in the failed boot. Citron
reaches Vulkan graphics-pipeline creation on AMD Radeon integrated graphics.
That confirms another execution path gets past initialization on this machine;
it does not prove the recomp runtime's services or renderer are complete.

## Generator/runtime fix

The generator inherited from LASRecomp/Let's Go can dispatch only to exact
function entries. Its ADR/ADRP and branch discovery can split an unwind
function into several emitted C fragments. An indirect switch target can then
land inside another fragment, where there is neither a callable entry nor a
usable local label in the source fragment. Unrecognized loaded BR targets can
also miss local labels. This is a reproduced failure class consistent with
the missing SDK address above.

The SDK is now generated with `--indirect-resume`:

- Every indirect BR tries the fragment's local labels before global dispatch.
- Unwind intervals containing BR instructions get static resume functions for
  their fragments, including destination fragments with no BR of their own.
- The library publishes a compact, sorted range table. On an exact-entry miss,
  dispatch accepts only aligned instruction addresses in these declared ranges.
- A resume function switches directly to that guest instruction in generated C.
  It preserves SP, LR and guest registers, without replaying the prologue.
- Ordinary direct calls still start at the regular entry; HLE entry points and
  overrides keep their existing behavior. Invalid/out-of-range PCs still fail.

No runtime interpreter or JIT is added. No guest code or assets are committed.
The table uses one record per fragment, rather than one entry per instruction.
Library ABI 4 prevents mixing this layout with older runtimes. The generator
hash also changes, so the runtime and game DLL must be rebuilt together.

## Updating an existing Windows build

Close the running game first. From the repository directory in PowerShell:

```powershell
git pull origin scarlet-bootstrap
Set-ExecutionPolicy -Scope Process Bypass
.\tools\scarlet_generate.ps1 -ExeFs "D:\Pokemon Scarlet\Extracted\Base\exefs" -Module sdk -Clean
if ($LASTEXITCODE -ne 0) { throw "Falha ao gerar SDK" }
.\tools\scarlet_build_windows.ps1 -Jobs 4
if ($LASTEXITCODE -ne 0) { throw "Falha no build" }
.\tools\scarlet_run.ps1 -GameRoot "D:\Pokemon Scarlet\Extracted\Base"
```

Only SDK C sources need regeneration. Keep generated main/subsdk0 sources and
their object files: their CPU-state ABI did not change. The build script
rejects SDK tables without nonzero resume metadata and prints the repair
command. `make gen` and the in-app builder also enable SDK resume generation.

The next boot should report `[dispatch] sdk: ... static resume ranges`. This
confirms the new metadata was loaded; it is not a claim that the game rendered.
If initialization completes, look for subsequent constructor messages and
`[gfx-milestone] nvnBootstrapLoader reached`. If another dispatch entry is
missing, the runner now inspects its exact PC and LR-4 automatically, before
the general host-backtrace inspection. The log remains `scarlet_boot.log` in
the repository directory.

## Verification and limits

```sh
python tools/test_atomic_lifter.py
python tools/test_indirect_resume.py
```

The new tests generate a synthetic NSO with an indirect jump into a different
fragment of the same unwind function. With resume metadata disabled, the
native runtime reproduces `no function at ...`. With it enabled, the actual
generated C, library entry and runtime dispatcher agree with ARM64 execution
in Unicorn for 128 combinations of branch target and register state. Tests
also check direct calls with a stale dispatch PC, SP/LR preservation, HLE
override precedence, invalid PCs and unwind boundaries. Native tests run on
Linux with Clang or Zig; discovery tests are portable.

These are regression tests for the reproduced generator failure, not a test of
Pokémon Scarlet reaching its menu. The original SDK/ROMFS and a Windows game
boot are unavailable in this development environment. Further CPU, service,
NVN or shader issues may become visible after the reported blocker is removed.
