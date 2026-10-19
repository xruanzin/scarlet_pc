# Transform-feedback widths: preserve the float-vector preprocessing pass

## Evidence

The frame-291 forensic capture contains two different output layouts under
the same NVN control signature (`10101/103`):

| Stage | Example shader | Export masks | Bytes per vertex in buffers 0 / 1 / 2 |
| --- | --- | --- | --- |
| Copy/morph preprocessing | `727303b000`, `7273062000` | xyz / xyz / xyz | 12 / 12 / 12 |
| Skinning and packing | `7273017e00`, `7273025c00` | xyz / x / x | 12 / 4 / 4 |

For example, draw 0 processes 108 vertices and binds three 1,296-byte TF
buffers. Its shader writes all three components of every output. Draw 120
reads that position buffer with stride 12 and produces 1,296 / 432 / 432 bytes.
The later rendering draws consume packed normal/tangent attributes as
`A2B10G10R10_SNORM_PACK32`.

The previous implementation forced **both stages** to 12 / 4 / 4. It captured
only the float-X bits in the normal/tangent streams of the preprocessing
stage, discarding Y/Z and writing each following vertex at the wrong offset.
Packing these floats in the first stage would also be wrong: the following
guest shader already performs the packing and expects float-vector input.

## Change

For the recognized three-stream signature, allow up to four components per
varying and derive each actual width from that shader's recorded export mask.
Use the same count for declarations, temporary initialization and final
assignments. Vector exports remain float vectors; scalar normal/tangent
exports retain the exact uint-bit path, including NaN-looking packed words.
The ShaderKey changes as well, selecting a new program variant.

Submission batching remains at 16. No texture-layout changes, game assets or
captured proprietary shader code are included in this fix.

## Validation

`tools/test_transform_feedback.py` generates synthetic shaders through the
production attribute-store and GLSL assembly functions. It covers vector,
packed and mixed layouts, compiles and validates their SPIR-V, and checks the
compiled XfbBuffer/XfbStride decorations. An optional Vulkan test reads back
all three streams, including untouched sentinel words and exact packed bits.

In this environment, the three non-GPU checks pass; the Vulkan readback is
skipped because no Vulkan driver is installed. The previous translator fails
the layout regression. Existing interpolation and 16-list batching regressions
also pass, and the edited Vulkan executor compiles.

The complete game and Windows AMD driver have not been run here. This fixes
an evidenced buffer-layout mismatch; visual completeness and FPS still need
verification on the user's machine.
