# Scarlet: condition publication and fragment interpolation

This update addresses two independently reproduced runtime defects. It does
not establish that Scarlet's complete 3D pipeline is implemented or playable.

## Evidence from the supplied run

`scarlet_boot(20261018-204037).log` stops presenting at frame 385 after the
keyboard returns the name `Q`. The guest thread snapshots at 8 and 60 seconds
show the same condition-variable waits, including the main thread at
`724214472c`. There is no recorded Vulkan device-loss error in that interval.
The snapshots establish stalled progress, but do not identify the lost signal
by themselves.

`build (3).zip` contains captures from multiple runs. Passes 074–085 are older
than passes 000–073, so they must not be treated as one frame of the supplied
log. A supplied vertex shader has generic attribute exports but no position
export. That can also be intentional for transform-feedback preprocessing;
inserting an invented position export would not implement that processing.

## Kernel fix

`svc_wait_process_wide_key` used to release the guest mutex and only then
publish `key = 1`. The SDK signal fast path can acquire the guest mutex and
inspect the condition key without taking the runtime's `g_klock`. Between those
two stores it can see zero and skip the signal. The waiter subsequently sleeps
with no signal pending.

Publish the condition key before releasing the mutex. Keep publication, mutex
handoff and wait-list insertion inside `g_klock`, so a signal entering the
kernel after unlock cannot overtake insertion. Do not inject periodic wakeups
or mark unfinished work as complete.

`tools/test_condition_publication.py` forces a second pthread to acquire the
guest mutex and inspect the flag exactly at unlock. The original source loses
the signal on the first iteration (`0xea01` timeout); the fixed source passes
100 forced interleavings, including reacquiring the mutex after the signal.

Semantics reference: [Atmosphere KConditionVariable](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp),
which publishes HasWaiterFlag before writing the released mutex value.

## Shader fix

The IPA implementation ignored its register multiplier and selected fragment
interpolation from the instruction's arithmetic mode. These are distinct:

- The SPH generic-input map selects flat, perspective or screen-linear inputs.
- A perspective host varying must be multiplied by PositionW to represent the
  Maxwell interpolator value.
- IPA.MUL additionally multiplies by the encoded register. The register must be
  read before the destination is overwritten, including aliased registers.
- Saturation applies after that calculation.

Read the SPH map, emit the corresponding host qualifier, perform the proper
perspective correction and register multiplication, and permit the IDX bit
with an RZ index (a direct load). Truly indexed attributes remain unsupported.
Old source-hashed shader-cache entries cannot override the changed GLSL source.

Encoding/semantics references:
[NVIDIA Shader Program Header](https://download.nvidia.com/open-gpu-doc/Shader-Program-Header/1/Shader-Program-Header.html)
and the Yuzu-derived Citron reference `shader_recompiler/program_header.h` and
`frontend/maxwell/translate/impl/load_store_attribute.cpp`.
The change is a direct implementation in the existing translator, without
importing an emulator graphics backend.

## Validation and limits

- New condition-publication test: passes with the fix, fails with the original.
- New interpolation tests: 72 numerical combinations, SPH qualifiers,
  destination/multiplier aliases, RZ, saturation and IDX/RZ pass. All three
  semantic tests fail against the original translator.
- 24 generated interpolation shaders compile with glslang and pass spirv-val
  for Vulkan 1.3.
- The existing texture-gather suite passes, including GLSL/SPIR-V validation.
- Linux syntax compilation covers the edited kernel, translator and command
  buffer source. No complete Windows executable or game execution was tested.

Broader pre-existing tests are not all green in this environment: the scene
suite has outdated 3-second expectations while production waits 30 seconds,
and a storage-view harness lacks VMA headers; the TLS harness's intentional
null-access test does not receive its expected SIGSEGV with this compiler.
The same failures reproduce in an untouched worktree at `3dfe4ee`.

The transform-feedback API stubs now log their first actual call under
`[nvn-unimplemented]`, including arguments and guest LR. They are still stubs.
A new log can distinguish actual use of this missing path from a harmless
no-position shader. This update does not claim to fix transform feedback.

## Windows retest

Use the existing `scarlet_build_windows.ps1` and `scarlet_run.ps1` workflow.
No generated game-code rebuild or cache deletion is required specifically by
these runtime changes. Test name entry and character customization normally.
If presentation stops again, allow the 60-second watchdog report to finish.

For a new graphics capture, remove only old diagnostic PNG/PGM files before
launching, press F9 once in customization, wait for capture to finish, then
close the game. Preserve the log and the new pass/depth files together with
`build/shader-diagnostics`, especially `nopos_*` GLSL and raw SASS dumps. Do not
mix them with previous passes or upload game executables/assets to the repo.
