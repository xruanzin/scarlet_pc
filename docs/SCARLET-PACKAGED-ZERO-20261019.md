# Preserve explicit zero packaged texture layout

The run `scarlet_boot(20261019-043456).log` reaches frame 1623 and closes the
window without the previous upload-command-buffer fatal error. It reports
grouped submissions and zero skipped draws in the late rendering intervals.
That establishes progress beyond the crash, not correct pixels or stable FPS.

## Bug and change

The texture builder used `layout_payload != 0` as its only presence test.
Consequently an explicit zero layout was indistinguishable from default/null
layout. In the existing supported layout encoding, zero selects one GOB per
vertical block; defaults instead derive geometry from texture dimensions.
Using the derived geometry for explicitly encoded zero changes the addresses
read during deswizzling, even if texture creation and all draws succeed.

Record presence separately in the unused final word at offset 0x5c, inside
the existing 0x60-byte host builder representation. Do not extend the object,
change the payload, or use the packaged-data pointer as a proxy for layout
presence. Defaults and a null layout clear the marker; a raw 96-byte builder
copy preserves it. The neighboring GL-name field occupies 0x58–0x5b.

The existing conservative treatment of unsupported nonzero layout encodings
is unchanged. No block height is guessed from a screenshot. This does not
alter the mip shrinking rules in tegra.c: storage sizing and effective mip
geometry must be considered separately, as in the reference implementations.

## Validation and limits

The native test drives actual builder setters and layout calculation against
an independently constructed 2-column x 4-row grid of 512-byte GOBs. Decoding
RGBA and BC7 blocks must reproduce their row/column values. The old source
fails before decoding because it discards explicit zero; the fixed source
passes. Other cases cover defaults, data without layout, null reset, all
supported nonzero heights, copied builders and a guard page immediately after
the 96-byte object. Sampler-builder and shader-control lifetime tests also pass.

No raw texture pixels or per-texture packaged-layout trace accompanied this
run, so the test does not establish that this specific bug explains all face
and scene corruption. Actual Windows/AMD rendering remains unverified here.
Set LARECOMP_LOG_PACKAGED=1 for the next run to see whether affected textures
select explicit zero. Additional unsupported layouts or shader issues may
require separate corrections. Sixteen-list batching remains enabled.

Reference used to check block-layout terminology and mip handling:
https://docs.rs/tegra_swizzle/latest/tegra_swizzle/
and the supplied Citron texture-cache implementation. No reference code is
copied into this change.
