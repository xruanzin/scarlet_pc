# Preserve the guest layer in split LUT draws

The first target-machine test of the individual-slice LUT path removed the
washed-out fallback but exposed a yellow/green cast. Frame 1287 provides the
missing shader evidence: VS `72731ea000` writes instance+1 to attribute `0x64`,
and FS `72731eb200` reads that attribute, subtracts one and uses it as the blue
coordinate of the colour-grading table.

Previously, the translator discarded the vertex store and replaced the
fragment read with zero. Selecting the correct physical attachment slice was
therefore insufficient. The earlier command/state tests did not cover this
shader-stage interface.

For the split LUT variant, carry attribute `0x64` as a flat unsigned varying
between the actual guest shaders. Keep its exact value, including the guest's
+1 convention; do not substitute the physical Vulkan layer, which is always
zero within each one-slice view. No geometry/layer-output feature is needed.

The variant is enabled only while executing the split LUT draws, restored on
both successful and rejected draws, and included in both ShaderKeys. Generic
location 31 is reserved for this interface in that variant; a conflicting
shader is rejected and uses the existing compatibility fallback. Ordinary
shader variants retain their previous interfaces.

Validation:

- Four `tools/test_layer_varying.py` checks generate both stages through the
  production attribute load/store and GLSL assembly paths; verify integer/flat
  transport; reject location collisions; compile, link and validate SPIR-V.
- The production LUT-splitting native test checks activation and restoration
  of the shader variant, including the rejected-draw path.
- Transform-feedback shader regressions pass and the Vulkan executor compiles.

These are native and compiler checks, not GPU/game execution. Verify final
colours on the user's Windows AMD system. The separate enrollment portrait
remains outside this patch; the supplied frame captures the cutscene.
