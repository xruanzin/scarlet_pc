# Compare the bound texture sources, not just emulator configuration logs

The supplied Citron/Eden/Ryubing logs identify the same Scarlet title and AMD
Radeon integrated GPU, using AMD proprietary driver 2.0.279. They contain
configuration, shader compilation/cache and synchronization messages, but no
per-draw texture bytes or shader inputs for the corrupted port frame.

Eden logs AMD float16 and sampler-minmax warnings; Citron logs a dynamic blend
equation workaround. The inspected port path does not select float16 arithmetic,
minmax reduction samplers or dynamic color-blend equations. These warnings alone
do not identify its rendering bug. The port's packaged-layout trace now contains
actual dimensions/formats/heights; explicit zero is selected, but the face and
intro scene remain visibly corrupt. The previous change is not a complete fix.

## F9 source capture

F9 now captures CPU source bytes for textures resolved from up to twelve sampler
references in each vertex/fragment stage of the forensic draws. Sources carry
format, shape, level count, stride, block geometry, channel swizzle, owning-pool
bounds and GPU-ownership metadata. All dynamically indexed descriptors and GPU
image contents are not captured by this path.

Only F9/explicit debug frames collect textures; ordinary skin logging does not.
Textures marked GPU-owned are metadata-only, because guest backing may be stale.
Invalid/incomplete pool ranges are also metadata-only. No GPU wait, readback,
upload or command-buffer mutation is introduced by source capture. Existing F9
pass screenshots/readbacks remain separate and can make capture slow.

Each capture has a fresh frame/time directory. Per-frame deduplication uses the
texture debug ID, with limits of 128 textures, 32 MiB per texture and 128 MiB total.
Files include the declared source plus bounded padding for alternative level-0
block-height analysis. Reads never extend past the owning guest pool. This is
CPU input evidence, not proof of the Vulkan image actually sampled by the GPU.

The collector resolves the runtime data directory from scarlet_boot.log and
selects the newest forensic report across that location and the repository build
directory. This handles the runtime build/build case. It includes exactly the
texture directory named by the chosen report, rather than globbing old captures.

## Validation

Native tests verify source bytes, deduplication, invalid pointers, short pools,
GPU-owned exclusion, texture/budget caps and frame isolation. PowerShell tests
run the actual collector with repository paths containing spaces, both runtime
root layouts, an older competing report and a stale texture directory. The ZIP
must contain the selected frame's bytes and exclude old textures. Edited Vulkan
code compiles; actual game/driver rendering is not validated here.

This commit adds evidence collection and fixes collection paths. It does not
claim to repair the remaining face/scene corruption or establish 30 FPS.

## Windows

Build normally, run the port, and press F9 once on the corrupted character screen.
Wait for the capture, close the game, then run tools/scarlet_collect_forensics.ps1.
Send the ZIP printed as FORENSICS_ZIP. It includes source textures, shaders,
draw metadata, pass screenshots and the log. A separate emulator rerun is not
needed for this collection. Captured game assets stay local until the user
chooses to share the diagnostic ZIP; they are not committed to the repository.
