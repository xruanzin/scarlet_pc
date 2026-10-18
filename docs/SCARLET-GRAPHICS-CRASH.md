# Scarlet: crash after main resumes and NVN window creation

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

## What the new crash establishes

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

The automatic address inspector prints only the beginning of the guest
function, not its full body, and the native offset cannot be converted to an
exact guest PC from that listing alone. Do not change a guessed NVN layout,
skip initialization or mask the invalid pointer on this evidence.

## Collect the missing instructions without rebuilding

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
