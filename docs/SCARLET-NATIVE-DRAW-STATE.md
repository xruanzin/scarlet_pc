# Native draw-state reuse

The renderer used to bind its graphics pipeline and re-emit depth, stencil,
blend and raster state for every draw, followed by viewport and scissor commands.
It now compares the resolved state and skips identical commands within a valid
recording scope. A separate exact-key shortcut reuses the last pipeline lookup,
avoiding repeated hashing of the vertex layout and other baked pipeline state.

## Lifetime and compatibility

- State is invalidated at command-buffer recording start, including reuse of the
  same Vulkan handle, and at the start of a new guest rendering pass.
- Clears that change dynamic state, post-processing, presentation and screenshot
  pipelines invalidate the tracker. Simple attachment clears do not change these
  states and retain the cache.
- A different pipeline conservatively re-emits all tracked dynamic state.
- In-place reboot clears both the last-pipeline shortcut and the dynamic tracker.
- Viewport/scissor comparisons include depth range, negative viewport height and
  signed scissor offsets. Shader and temporal-upscaler inputs remain unchanged.
- SCARLET_DRAW_STATE_REUSE=0 restores unconditional state emission for diagnosis.
  It does not change resolution, shaders, cache files or AOT modules.

The lifetime rules follow the Khronos documentation:
[dynamic state](https://docs.vulkan.org/guide/latest/dynamic_state.html).
The existing AMD fast-pipeline creation policy is preserved: the repository's
SCARLET-BOOT-RECOVERY.md records severe boot regressions from compilation work.
This change does not blindly enable the previously rejected shaderc optimization.

## Validation and limits

```text
python tools/test_vulkan_draw_state.py
python tools/test_vulkan_pipeline_lookup.py
```

The production state emitter runs against a recording Vulkan mock. Tests verify
every tracked field, repeated identical draws, command and pipeline changes,
foreign state invalidation, and the uncached diagnostic path. In the mock, 1,000
repetitions of identical stencil-enabled state add no driver calls after the
initial 20 state commands and one pipeline bind. That is a command-count result,
not a gameplay FPS measurement.

The pipeline lookup test checks every byte of the full key, the normal table
path, repeated keys and reboot invalidation. Windows CI additionally builds the
complete executable and exercises native AOT DLL loading.

These changes address CPU/driver overhead. The existing captures also show long
Vulkan waits; neither GPU execution cost nor the remaining colour differences
are established as solved by these tests. Game assets and the user's AMD driver
are not available in CI.
