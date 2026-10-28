# Native Vulkan upload and binding changes

These changes target repeated CPU/driver work in the existing native renderer.
They do not change shaders, resolution, temporal inputs, presentation, AOT DLLs,
or the GPU completion conditions required for resource reuse.

## Implementation

- Guest upload lookup uses at most four probes instead of a single direct-mapped
  entry. Equal addresses and lengths reuse a snapshot only within the same
  submission and guest-write generation. Submission and generation are separate
  fields instead of an arithmetic combination that could alias.
- Missing uniform buffers and fallback vertex streams reuse their immutable zero
  data uploads. Storage address maps reuse a copy only after a byte comparison,
  independently for vertex, fragment and compute stages.
- Contiguous vertex bindings use one Vulkan command per run. Sparse holes remain
  untouched. The existing direct guest-buffer path remains in place; no policy
  that assumes dedicated GPU VRAM is introduced.
- Descriptor-key initialization touches only active bindings, instead of clearing
  192 entries for every stage of every draw. The exact identity comparison and
  existing SCARLET_DESCRIPTOR_REUSE=0 diagnostic opt-out remain unchanged.

## Validation

Run from the repository root with Python 3 and a C compiler available:

```text
python tools/test_vulkan_upload_cache.py
python tools/test_vulkan_vertex_bindings.py
python tools/test_vulkan_descriptor_keys.py
```

The tests compile the production helper functions, substituting only the GPU API:

- Four colliding ranges remain cached across 1,000 alternating requests. Guest
  writes, length changes, submission changes and epoch aliasing are checked.
- All 131,072 masks of 17 vertex slots preserve the individual buffers/offsets,
  bind every active slot exactly once, and never bind unused holes.
- Every descriptor identity field is checked, unsupported binding forms fall back,
  and key construction leaves unused entries untouched.

The same tests run in native CI alongside the Windows executable build and AOT
loader smoke tests. These are correctness and command-count checks, not gameplay
FPS measurements. The user's existing captures show substantial Vulkan waiting;
these changes alone do not prove that the dominant GPU bottleneck is removed.
Colour differences and the earlier svcBreak remain separate unresolved issues.
