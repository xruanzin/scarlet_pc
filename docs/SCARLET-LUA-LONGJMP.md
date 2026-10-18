# Scarlet: protected-call return at main+0x26c94

The supplied `scarlet_boot(20261018-034105).log` passes the sampler overflow,
reads more archives (over 50 MB reported), loads the system font and mounts
saves. It later fails with:

```text
FATAL: no function at 7000026c94 (lr=7000026c94 ...)
guest: sp=723ffff2a0 lr=7000026c94 ...
x0=1 x17=70065ad9f4 x29=723ffff3f0
```

The automatic disassembly shows a Lua-style protected-call frame at
`0x7000026c50`. It calls the setjmp thunk `0x7003017b70` at `0x7000026c90`,
tests its result at `0x7000026c94`, and invokes a callback on the initial zero
return. Its error path calls `0x7003017b60` with the saved context buffer and
value 1. At failure the guest SP, callee-saved registers and LR have been
restored and execution is transferring to that nonlocal return PC.

## Why another resume address alone is insufficient

The old resume discovery covered unwind intervals containing indirect BR
switches. This protected-call function has BLR, but no such BR. Its setjmp
return PC therefore had no resume entry. More importantly, SDK longjmp
restores the **guest** stack; the abandoned recompiled C callback frames are
still on the **native** stack. Dispatching a continuation and then returning
into those abandoned frames would execute code that should have been skipped.

## Generated native scopes

The generator recognizes calls to the verified setjmp thunk only for main
Build ID `ca44e7dd62e61a2a4e8889b3319ae173375770fd`. Other builds do not inherit
that address. A developer can supply `--setjmp-target` for a separately
verified target or a synthetic test.

Each recognized call tail-enters a small generated helper. The helper calls
`rt_guest_setjmp`, which keeps a native jump buffer live while it runs the
original SDK setup and then the statically generated continuation. A later
SDK longjmp still restores the original guest context using the original SDK
instructions. The dispatcher matches CPU pointer, saved SP and return PC to
the active native scope, unwinds abandoned native calls and resumes the
continuation. Ancestor native callers below that scope remain live and return
normally. A return PC that begins a separate generated fragment uses its
ordinary entry; an interior PC uses its resume entry.

Scopes form a thread-local stack, allowing a jump to an inner or outer
protected call and repeated reuse. No guest jmp_buf layout is guessed or
replaced. SDK instructions, CPU-state layout and game-library ABI remain
unchanged. The generator hash changes, so runtime and game DLL must be built
together. The first few nonlocal transfers per thread log:

```text
[dispatch] guest longjmp -> pc=7000026c94 sp=... value=1
```

## Incremental Windows update

Close the game. From the repository folder:

```powershell
git pull --ff-only origin scarlet-bootstrap
Set-ExecutionPolicy -Scope Process Bypass
.\tools\scarlet_generate.ps1 -ExeFs "D:\Pokemon Scarlet\Extracted\Base\exefs" -Module main
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1
```

Regenerate main only, without `-Clean`; retain SDK/subsdk0 sources and objects.
The generator preserves unchanged chunks. The build wrapper requires a
nonzero `main_setjmp_site_count` so the old main cannot silently be reused.
Do not run until the build prints `BUILD CONCLUIDO`.

## Verification and remaining limits

`python tools/test_guest_setjmp.py` generates synthetic protected-call code
and links it with the production dispatcher and game-library entry. It checks
normal callback return, zero/nonzero jump values, inner and outer nested jumps,
the ancestor continuation running exactly once, repeated scope reuse and four
concurrent threads. It exercises both an interior continuation and one split
into a separate fragment. The old build reproduces the missing return PC;
adding that PC alone reproduces execution of an abandoned callback. Both
complete fixes pass. A second test verifies exact Build-ID/module scoping.

This repairs the nonlocal control transfer. It does not establish why the
game raised the original Lua error or that it now reaches the menu. The next
Windows boot must show whether error handling completes or exposes a further
missing service, script/resource or graphics implementation.
