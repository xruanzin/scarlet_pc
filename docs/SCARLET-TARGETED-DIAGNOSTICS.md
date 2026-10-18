# Targeted Scarlet diagnostics: NVN controls and HLE wall time

The run `scarlet_boot(20261018-210733).log` confirms actual calls to the
unimplemented transform-feedback binding, begin and end APIs. Existing F9 and
no-position dumps include shader instructions, but not the shader control block
passed to `nvnProgramSetShaders`. Implementing the output-buffer layout requires
examining that metadata rather than assuming a tightly packed attribute order.

The same profile contains 771 slow-frame reports from frame 1300 onward. Their
median reported frame duration is 261 ms, executor time 20.6 ms, presentation
0.4 ms and time outside measured executor/presentation 222.7 ms. The median
outside fraction is 92%. These are independent medians, not one additive frame.
They also describe logged slow frames, not every frame of the run.

The outside category can include guest execution, HLE work, synchronization,
scheduling and work on other threads. It is not a measurement of game logic
alone. The existing timeline-wait counter can exceed measured executor time
because it includes waits from other renderer entry points; do not add those
counters together or subtract all waits from the executor as one CPU budget.

## What this update implements

- `-Profile` additionally measures completed HLE calls, reporting the ten largest
  inclusive wall-time totals at the normal renderer statistics interval.
  Nested callbacks and parallel threads can overlap, so their totals are not
  CPU utilization. Guest code and direct overrides outside the HLE stub path
  are not timed by this probe.
- `-GraphicsDiagnostics` saves the known 0x704-byte control prefix for the
  first 128 non-fragment graphics shaders. For programs actually active at
  transform feedback, it recognizes the standard NVN 0x880-byte control from
  the control's own +0x6f0 size field and saves the full control, 0x80-byte
  NVN/SPH header and bounded raw code block. It also records up to 128 TF API
  calls. Early UI programs cannot consume the independent TF-program quota.
- Shader controls are deduplicated by code/control/stage. The collector extends
  beyond 0x704 only when the already-known prefix advertises the standard
  0x880-byte control size. Raw code capture additionally validates a bounded
  code size before writing it. Normal launches disable all of these dumps.
- The first-128 inventory keeps only a small bounded set of snapshots. Late
  shaders are not retained speculatively. At Begin/ResumeTransformFeedback the
  currently bound program is captured on use, which avoids the previous 16 MiB
  live-snapshot budget being exhausted before Scarlet reached customization.
- On exit the launcher collects this run's log, the captured controls, the
  no-position GLSL/SASS files named in that log and an executable SHA-256 into
  a fresh timestamped ZIP. Older diagnostic files are not included by globbing
  an entire shader directory. Exit code and previous environment are preserved.

This update is instrumentation. It does not implement transform feedback,
claim a rendering fix or promise an FPS increase.

## Windows collection

```powershell
cd "C:\Users\ruana\Downloads\scarlet_pc"
git pull --ff-only origin scarlet-bootstrap
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\tools\scarlet_build_windows.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\tools\scarlet_run.ps1" -Profile -GraphicsDiagnostics
```

Enter customization, stay there for about 20–30 seconds and close normally.
Do not press F9 during this performance sample. Send the ZIP whose absolute
path the launcher prints after exit:
`build/scarlet-diagnostics-<timestamp>.zip`.

## Validation

- Native tests verify the 0x704 ordinary prefix, 0x880 TF control capture,
  bounded raw-code dumps, disabled-path non-dereference, deduplication,
  shader/call caps and exclusion of fragment/compute controls. They fill the
  initial quota before binding late TF programs and check both binding orders
  and the separate quota.
- Native HLE tests verify preservation of function results and PC changes,
  nested-call accounting, 16,000 concurrent calls and interval reset.
- PowerShell 7 tests exercise normal and abnormal process exit, both profiling
  modes, output ZIP contents, actual runtime data-root resolution, exclusion of
  stale shaders and environment restoration. The native game is mocked.
- The edited C translation units compile on Linux; the existing watchdog tests
  also compile the Windows platform against the Zig Windows SDK. The actual
  Scarlet Windows executable and AMD driver execution are not tested here.
