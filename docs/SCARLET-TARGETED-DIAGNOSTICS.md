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
- `-GraphicsDiagnostics` saves a known shader-control prefix (0x704 bytes) and
  the NVN/SPH header (0x80 bytes), plus constants offsets and sizes. It captures
  up to 128 distinct non-fragment graphics shaders and 128 TF API calls.
- Shader controls are deduplicated by code/control/stage. The collector does
  not follow embedded pointers or guess larger allocations. Normal launches
  disable these dumps and the added per-call timing.
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

- Native tests verify control-prefix sizes, disabled-path non-dereference,
  deduplication, shader/call caps and exclusion of fragment/compute controls.
- Native HLE tests verify preservation of function results and PC changes,
  nested-call accounting, 16,000 concurrent calls and interval reset.
- PowerShell 7 tests exercise normal and abnormal process exit, both profiling
  modes, output ZIP contents, actual runtime data-root resolution, exclusion of
  stale shaders and environment restoration. The native game is mocked.
- The edited C translation units compile on Linux; the existing watchdog tests
  also compile the Windows platform against the Zig Windows SDK. The actual
  Scarlet Windows executable and AMD driver execution are not tested here.
