# Cutscene deferred-lighting stencil sampling

## Capture evidence

Frame 1041 shows the scene silhouette in pass 16, but its lighting already has
speckled and striped material colours. Later bloom/tone-map passes amplify
that corruption; subtitles are composited afterwards and remain readable.

Draw 378 (`VS 72730ba000 / FS 72730f4000`) binds the same D32/S8 NVN texture
(`726690c000`, format `0x35`) at two different texture slots. The first fetch
is treated as integer material flags: bit 6 controls discard, and bits 0..2
select material branches including IDs 4, 5, 6 and 7. The other fetch is used
as floating-point depth for position reconstruction.

The backend ignored both the texture and view depth/stencil sampling mode:
every sampled D32/S8 view selected depth. The translator also declared all
these samplers as floating point and bitcast the sampled float to a register.
Consequently, material classification consumed depth-mantissa bits instead of
the stencil byte. This is a concrete incompatibility consistent with the
first corrupt lighting pass; the full visual result still needs a game test.

## Implementation

- Honour stencil mode on combined depth/stencil textures, including explicit
  depth-mode overrides. Stencil-only textures select stencil as well.
- Include a mask of direct NVN stencil bindings in each graphics ShaderKey.
  Resolve the binding from the shader handle offset, independently of the
  translator's texture-slot allocation order.
- Emit unsigned stencil samplers and transfer their integer results directly
  to registers. Ordinary depth sampling remains floating point.
- Keep separate depth and stencil views of attachment feedback snapshots.
  Copy both planes with separate single-aspect VkImageCopy regions, rather
  than one invalid combined-aspect region.
- Mark stencil texture references in F9 forensic records.

This patch does not modify the transform-feedback fix or submission batching.
It targets direct graphics texture bindings; dynamically indexed bindless
texture sampling and broader integer colour-texture support are separate work.

## Validation

`tools/test_stencil_sampling.py` executes the production view selection,
binding mask and snapshot helpers with captured Vulkan arguments. It checks
default depth, explicit stencil, inherited stencil, explicit depth overrides,
attachment aspects, two-plane snapshots and colour-only snapshots. Synthetic
GLSL generated through the production translator verifies a stencil slot and
a separate depth slot, including nonmatching binding/translation indices.
The modules compile and validate with glslangValidator and spirv-val.

All three checks pass. Selecting the old texture backend fails at the expected
stencil-aspect assertion. Transform-feedback layout checks and the 16-list
batching regression pass, and both edited Vulkan translation units compile.
GPU readback and the actual Windows/AMD game are not available in this host.
