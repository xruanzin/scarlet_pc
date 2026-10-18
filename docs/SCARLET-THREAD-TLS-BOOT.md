# Scarlet: writable thread pointer missing during SDK constructors

## New boot evidence

The boot log `scarlet_boot(20261018-022909).log` confirms the preceding SDK
dispatch fix was loaded:

```text
[dispatch] sdk: 16739 static resume ranges
```

The old missing-entry failure is absent. This run instead faults at address
`0x20` in the recompiled main-module `malloc` at `0x7002776c80`, called during
SDK initialization. The supplied instruction inspection identifies the cause:

```text
movz x0, #0, lsl #16
movk x0, #0x20
mrs  x10, tpidr_el0
ldr  x9, [x10, x0]
```

At the fault, `x0=0x20` and `x10=0`. The writable thread pointer was never
initialized, so the per-thread allocator slot is read at `NULL + 0x20`.
The kernel's `thread_new` set only `cpu.tpidrro`, leaving `cpu.tpidr` at its
calloc default. The lifter already maps MRS/MSR correctly to these separate
Cpu fields. This failure still precedes the NVN graphics milestone.

For the general AArch64 writable thread-pointer convention, see the primary
[Arm AArch64 System V ABI, thread-local storage section](https://github.com/ARM-software/abi-aa/blob/main/sysvabi64/sysvabi64.rst).
The diagnosis above comes from this port's code and the supplied register dump;
it does not assume that the entire Horizon TLS layout is System V compatible.

## Runtime change

- Each newly created guest thread receives its own zero-initialized 8 KiB
  bootstrap area for `TPIDR_EL0`, before it can execute constructors or malloc.
- This area is separate from the existing 0x200-byte `TPIDRRO_EL0` TLR/IPC
  slot, from every other thread's area, and from the application/HLE heaps.
- The writable pool has the same 2048-slot capacity as the existing TLR pool.
  Creation checks bounds before writing memory. Slot allocation and thread ID
  assignment now share the kernel lock so concurrent creations cannot collide.
- Guest MSR instructions can replace the bootstrap pointer normally; starting
  a thread does not reset the pointer or inherit another thread's malloc cache.
- Reused slots are cleared on the next thread creation after a kernel reset.
- The main-thread startup log prints both pointers. Crash reports also include
  `tpidr` and `tpidrro`, making later TLS changes visible.

This supplies the missing bootstrap storage for the observed local-exec TLS
access. It does not implement arbitrary module TLS initialization templates or
all dynamic TLS relocation forms. Those need actual module metadata and will
require separate work if later execution uses them. No guest allocator code is
skipped and no NULL memory access is converted into a successful result.

## Rebuild on Windows

Close the game and run from the repository directory:

```powershell
git pull --ff-only origin scarlet-bootstrap
Set-ExecutionPolicy -Scope Process Bypass
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1
```

Reuse the SDK, main and subsdk0 sources and the game DLL from the preceding
update. This patch changes neither generated code nor the game-library ABI/ID
inputs. The build recompiles the runtime and keeps compatible game objects.
The new log should include:

```text
[kernel] main thread TLS: TPIDR_EL0=7090100000 TPIDRRO_EL0=7090000000 bootstrap=0x2000 bytes
```

This line confirms bootstrap setup, not completion of the game's boot. Inspect
the next `scarlet_boot.log` for subsequent constructors and graphics milestones.

## Verification

```sh
python tools/test_thread_tls.py
python tools/test_indirect_resume.py
python tools/test_atomic_lifter.py
```

The Linux native TLS harness compiles the real `runtime/kernel.c` with the
existing lifter's translation of the supplied malloc instruction sequence.
It reproduces SIGSEGV with `tpidr=0` and successfully reads the allocator slot
with a real initialized kernel thread. It also checks a nonzero cache slot,
IPC preservation, guest MSR replacement, separate per-thread state, 256 thread
creations from eight host threads, zeroed reuse after reset, and exhaustion of
the 2048-slot pool before an out-of-bounds write.

The Windows game boot and original game files are unavailable in the test
environment. Passing these tests verifies the reported TLS access and kernel
allocation behavior; menu/gameplay/image output still requires a new game run.
