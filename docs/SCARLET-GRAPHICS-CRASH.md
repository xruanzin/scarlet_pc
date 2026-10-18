# Scarlet: crash after main resumes and NVN window creation

## Confirmed cause and runtime fix

The supplied `scarlet_graphics_report.txt` identifies the actual fault as
`main+0x7e5ef0`, a zero pair-store through `X29-0x20`. The corresponding native
instruction is `vmovups xmmword ptr [rax-0x20], xmm6` at DLL RVA `0x4c6fee2`.
RAX was loaded from `Cpu.x[29]`, confirming the invalid frame pointer is the
address used by the store, rather than a GPU/driver pointer.

The caller at `0x7000851738` allocates a 0x70-byte frame and passes SP as the
sampler-builder address. It saves X29/LR at SP+0x40 and SP+0x48, leaving exactly
0x40 bytes for the builder. The old HLE stored a device pointer followed by
the entire 72-byte **host** `NvnSamplerDesc`, taking 80 bytes in guest memory:

| Builder offset | Old HLE field | Actual guest frame field | Observed result |
| --- | --- | --- | --- |
| `+0x40..+0x47` | last two border floats | saved X29 | `3f8000003f800000` |
| `+0x48..+0x4f` | host backend cache pointer | saved LR | `0` |

Defaults zeroed the backend field; setting the white border wrote the two 1.0
float bit patterns. The caller then restored those overwritten values, exactly
matching the register dump. The next store through X29 faulted.

The runtime now uses its own 64-byte opaque guest builder with packed enum
fields, float settings and raw border bits. A compile-time size assertion
enforces the boundary. Both `nvnSamplerInitialize` and direct builder-to-pool
registration explicitly convert it to the unchanged host descriptor, whose
backend cache starts NULL. Defaults preserve the already-set device.
No shader, guest instruction, CPU ABI or generated game code is changed.

Close the game, then update and rebuild the runtime:

```powershell
git pull --ff-only origin scarlet-bootstrap
Set-ExecutionPolicy -Scope Process Bypass
.\tools\scarlet_build_windows.ps1 -Jobs 4
.\tools\scarlet_run.ps1
```

Reuse the generated main/SDK/subsdk0 sources, objects and game DLL. This patch
changes only runtime NVN code and leaves GAMELIB_ID inputs unchanged.

`python tools/test_sampler_builder.py` compiles the production HLE and executes
the report's lifted stack prologue/epilogue and following store. The same test
against the retained old objects.c fails with **exactly**
`fp=3f8000003f800000 lr=0`; the fixed code restores both registers and completes
the store. It also places a 64-byte builder immediately before an inaccessible
page to catch overreads/writes, checks getters, raw signed/unsigned border
bits, device preservation, copying all builder state in 64 bytes, sampler
initialization, and both descriptor-pool registration paths.
Full Windows boot and image presentation still require the next game run.

## Earlier boot evidence

The supplied `scarlet_boot(20261018-032753).log` confirms the updated DLL
loaded successfully after the response-file link fix:

```text
[dispatch] main: 49860 static resume ranges
[dispatch] sdk: 16739 static resume ranges
[nvn] window with 2 textures
```

It passes the old `main+0xa7db34` dispatch stop, reads more game archives,
creates the NVN window and starts the audio renderer/device. This is boot
progress; the log does not establish an image was presented.

## What the initial log established

The host access violation is inside `f_70007e59a4+0x1182` at
`lasgame.dll+0x4c6fee2`. The guest dump contains:

```text
sp=723ffff4a0 lr=0 tpidr=7240001140 tpidrro=7090000000
x0=3f8000003f7fffd0 x8=ffffffffffffffff
x29=3f8000003f800000 x30=0
```

The two 32-bit halves of X29 have the bit pattern for float 1.0. X29 is a
guest frame-pointer register, and LR is zero. This is consistent with
overwritten/restored stack state; it does not identify which store or call
first corrupted that state. The last recorded HLE call is
`nvnSamplerInitialize`, but the HLE ring does not record intervening guest
instructions and cannot establish that the sampler implementation caused it.

The automatic address inspector printed only the beginning of the guest
function, not its full body, and the native offset cannot be converted to an
exact guest PC from that listing alone. Do not change a guessed NVN layout,
skip initialization or mask the invalid pointer on this evidence.

## Collect the missing instructions without rebuilding

This read-only collection produced the report used for the fix above. It is
retained for other graphics failures; no new report is needed before testing
this runtime correction.

`scarlet_graphics_report.py` reads the existing NSOs, boot log and game DLL.
It collects the guest unwind regions around the first crashed frame and the
last NVN call sites, their ARM64 instructions and portable-C lifting, and the
exact x86-64 fault instruction from the DLL's PE exception table. If that table
has no entry, it can use a log frame naming the exact host fault address.
It never starts x86 disassembly at an arbitrary PC-minus-offset byte boundary.

The DLL must be the one that produced the log. Collect before rebuilding:

```powershell
git pull --ff-only origin scarlet-bootstrap
.\.venv\Scripts\python.exe .\tools\scarlet_graphics_report.py "D:\Pokemon Scarlet\Extracted\Base\exefs" --output .\scarlet_graphics_report.txt
```

Run from the repository directory. Defaults are `scarlet_boot.log` and
`build/lasgame.dll`; `--log` and `--dll` select a retained matching pair.
Send `scarlet_graphics_report.txt` for analysis. No SDK/main regeneration,
runtime rebuild or game launch is required. Only the report text is written;
input DLL/log paths, their aliases, and output inside ExeFS are rejected.
No runtime behavior is changed by this diagnostic update.

## Verification

`python tools/test_graphics_report.py` uses synthetic NSOs and an x86-64 PE
fixture. Four tests cover PE RVA/exception mapping, the exact fault instruction,
UTF-16 PowerShell logs, guest disassembly/lifting, missing/different DLLs,
input-hash preservation, CLI output and rejected input overwrites.
