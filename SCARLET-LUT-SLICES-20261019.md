# Render the game's colour-grading LUT into individual depth slices

## Evidence and scope

Frame 1376 now has coherent material colours in lighting pass 103. The strong
washed-out appearance begins in pass 168, whose shader samples the 32-cubed
colour-grading LUT. Its input is logarithmically encoded before the lookup;
the previous straight RGB-ramp replacement is not an identity operation in
this colour space. It bypasses the game's actual grading curve and constants.

The enrollment-photo rectangle in the supplied screenshot is a separate open
issue. The packaged pass/texture capture is from the academy establishing shot,
not that rectangle. This change does not claim to repair portrait capture.

## Implementation

Execute the original guest LUT draw as 32 single-instance draws, binding one
2D-array attachment view of one 3D depth slice at a time. Preserve each original
instance index through Vulkan firstInstance. Each draw uses the guest vertex
and fragment shaders and current uniforms; it does not invent a replacement
tone curve. Restore all render-target state and leave it dirty afterwards.

This avoids layered 32-slice rendering, previously disabled after an AMD driver
fault. Only the recognized single-target 32x32x32 A2B10G10R10 LUT is split.
Already explicit slices and partial views are left alone. The old identity
fallback remains available with `LARECOMP_LUT_IDENTITY=1`, and for unsupported
instance configurations of that LUT. Shader/pipeline rejection also falls back
instead of leaving the LUT uninitialized; partial draws are completed first.

Fix attachment-view bounds as well: a Vulkan 3D image has one array layer, but
a 2D attachment view addresses its mip's depth slices. Clamping against the
array-layer count had collapsed every nonzero slice to slice zero. Sampled
3D views still use one array layer; ordinary 2D/array views retain their bounds.

## Validation and limits

- `tools/test_lut_slices.py` executes the production split function with draw
  stubs, checking all 32 slice/instance pairs, indexed and nonindexed arguments,
  recursion termination, state restoration, partial-view exclusion and fallback.
- The native view test in `tools/test_stencil_sampling.py` now checks all 32
  attachment slices and mip-level depth bounds. The old texture backend fails
  the nonzero-slice assertion. Its stencil and generated GLSL/SPIR-V checks pass.
- Both edited Vulkan translation units compile.

GPU execution of the slice path and the full Windows/AMD game are not available
on this host. The tests establish command/state correctness, not final colours,
driver stability or a specific frame rate. Verify the cutscene on the target PC.
