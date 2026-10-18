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

## Follow-up: the SDK path replaces the bootstrap pointer

The subsequent `scarlet_boot(20261018-024121).log` shows both:

```text
[kernel] main thread TLS: TPIDR_EL0=7090100000 TPIDRRO_EL0=7090000000 bootstrap=0x2000 bytes
guest: ... tpidr=0 tpidrro=7090000000
```

The bootstrap allocation was loaded, but the writable pointer became zero
later in initialization and the malloc fault persists. This means the
creation-only change is insufficient for the real boot, despite passing the
isolated malloc-access test. The log does not identify the guest instruction
that replaced the pointer or establish the intended rtld/SDK TLS contract.

Collect that missing evidence directly from the matching extracted NSOs:

```powershell
git pull --ff-only origin scarlet-bootstrap
.\.venv\Scripts\python.exe .\tools\scarlet_tls_report.py "D:\Pokemon Scarlet\Extracted\Base\exefs" --output .\scarlet_tls_report.txt
```

The report lists every `MSR TPIDR_EL0,Xn` site, its instruction context,
candidate function starts/callers, available libc startup symbols, SDK DT_INIT,
and the function address observed in x0 at the no-op `rtld_mark` callback. That
argument alone does not establish the callback's intended semantics. The
fixed-address contexts are enabled only for the SDK Build ID from the log.
The report distinguishes candidate instructions from proof of execution.

No SDK regeneration, runtime rebuild or game execution is needed to collect
this report. The tool reads NSOs and writes diagnostic text outside ExeFS; it
does not change the executable, reset TLS on every read, or suppress the crash.
The report has now identified the missing publication step, described below.
Synthetic tests verify exact MRS/MSR discrimination,
zero-register writes, direct callers, Build-ID scoping, unchanged input hashes,
and rejection of an output path that would overwrite a game file.

## Report result: the SDK reloads the TLR's ThreadPointer slot

The supplied `scarlet_tls_report.txt` finds only three TPIDR_EL0 writes across
main/sdk/subsdk0. Two SDK paths use exactly this sequence:

```text
MRS X8, TPIDRRO_EL0
LDR X8, [X8, #0x1F0]
MSR TPIDR_EL0, X8
```

The main-thread path writes at `0x700637e110`, in the SDK thread-manager
constructor; the worker entry writes at `0x700637e40c`. The host initialized
`Cpu.tpidr` but left the source slot at `TLR+0x1F0` zero. Those reloads therefore
discarded the otherwise valid bootstrap pointer. The SDK's third writer,
`0x700640dbf0`, publishes its own nnmusl TLS pointer to **both** the TLR slot
and TPIDR_EL0. This establishes the source/mirror contract for this SDK.

Thread creation now publishes each thread's bootstrap pointer into its own
`TLR+0x1F0` before guest code runs. Guest MRS/MSR instructions are unchanged.
When nnmusl initializes a replacement block and updates the slot/register,
subsequent SDK reloads use that replacement normally. The neighboring
ThreadType field at `+0x1F8` and the IPC command buffer remain independent.

The main-thread startup log now prints `tlr[0x1f0]` as well as both registers;
the initial slot value must equal TPIDR_EL0. This patch still needs only a
runtime rebuild and reuses the generated SDK/main/subsdk0 and game DLL.

### Citron reference

The supplied Citron repository was inspected at commit
`598b700884c7cd3bee0a25ce30d0cab066117c5b`:

- [k_thread.cpp](https://github.com/citron-neo/emulator/blob/598b700884c7cd3bee0a25ce30d0cab066117c5b/src/core/hle/kernel/k_thread.cpp)
  allocates and zeroes the TLR for a user thread, separately from its CPU context.
- [k_process.cpp](https://github.com/citron-neo/emulator/blob/598b700884c7cd3bee0a25ce30d0cab066117c5b/src/core/hle/kernel/k_process.cpp)
  creates the main thread at the process entry point and sets its initial arguments.
- [deconstructed_rom_directory.cpp](https://github.com/citron-neo/emulator/blob/598b700884c7cd3bee0a25ce30d0cab066117c5b/src/core/loader/deconstructed_rom_directory.cpp)
  loads `rtld` first at that entry point, preserving the original loader path.

These distinguish kernel thread storage from the later SDK initialization.
This port replaces rtld with host callbacks, so it must publish its bootstrap
state in the fields that the remaining original SDK code actually reads.
The missing `+0x1F0` publication is established by the user's own SDK report;
it is not a claim that Citron seeds a custom bootstrap pointer in that slot.
The change is implemented in this port's existing native runtime.

### Extended regression

`test_thread_tls.py` now executes the actual lifted SDK reload sequence followed
by the observed malloc load. A valid Cpu.tpidr plus an empty TLR slot reproduces
the reset to zero. With the published slot, reload and malloc succeed. A second
probe exercises nnmusl's observed ADD/STR/MSR publication and verifies that its
replacement pointer survives later reloads. Checks also cover a direct guest
MSR that does not update the slot, per-thread mirror isolation, IPC/ThreadType
preservation, concurrent creation and zeroed/re-published slots after reset.

## Runtime change

- Each newly created guest thread receives its own zero-initialized 8 KiB
  bootstrap area for `TPIDR_EL0`, before it can execute constructors or malloc.
- Its TLR ThreadPointer field at `+0x1F0` publishes the same pointer for SDK reloads.
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
[kernel] main thread TLS: TPIDR_EL0=7090100000 TPIDRRO_EL0=7090000000 tlr[0x1f0]=7090100000 bootstrap=0x2000 bytes
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
