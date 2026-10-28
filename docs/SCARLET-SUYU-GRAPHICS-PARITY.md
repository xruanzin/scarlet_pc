# Scarlet PC — visual parity with the suyu build

**Priority:** Match suyu's image fidelity before optimizing FPS. The native
runtime should retain the original guest textures and color-space semantics,
shader-based lighting, depth of field, grading LUT and material effects.
The current native build has NOT been shown to be pixel-identical to suyu.

## Why the baseline is necessary

The native renderer currently includes some **port-authored** approximations:
an isotropic hair lighting workaround, an academy/schoolyard grade override,
SMAA/FSR1 after the guest image, and optional FSR3/FSR4/DLSS/interpolated
presentation. These can conceal native translation defects and make a
direct visual comparison unreliable.

The opt-in `-GraphicsReference` mode uses `SCARLET_GRAPHICS_REFERENCE=1`.
It bypasses these extra native interventions **for the current process only**,
while retaining the **original** guest LUT (32 slices), material shaders,
depth-of-field, sRGB texture formats and host NVN->Vulkan translation.

**Important:** Reference mode is a diagnostic A/B baseline, not a completed
repair or a claim that unmodified guest shaders already look right. It can
expose bugs formerly concealed by native compatibility workarounds. The
ordinary mode remains unchanged, including the default hair workaround.

## Launch using the existing AOT + prepared update

Run inside the repository; keep `-BuildRoot` on D: to avoid the full C: disk.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File ".\tools\scarlet_native_run.ps1" `
  -Runtime native `
  -GamePath "D:\ScarletNativeCache\scarlet-8df208305cfada13" `
  -BuildRoot "D:\ScarletNativeCache\native-build" `
  -GraphicsReference -Build -Jobs 2 -NoLauncher
```

The runtime log should include
`[graphics-reference] raw guest output; no native temporal upscale...`.
This does not rebuild the four AOT DLLs.

For comparison, launch without `-GraphicsReference` and keep the
`-Upscaler` value equal to whatever you normally use. Do **not** compare
a 720p raw reference capture to a 1080p FSR-upscaled or sharpened capture.

## Capture comparable frames

1. Use the same game version, save, weather, in-game time, camera orientation,
   resolution, aspect ratio, and display/gamma settings in both suyu and native.
2. Capture the **same gameplay frame** in PNG if possible. Disable overlays,
   frame generation, external filters, sharpening and recording color effects
   for the baseline (or match them exactly).
3. In the native renderer, the existing F9 forensics can also capture
   individual material MRT attachments, LUT output and texture-source bytes.
   These provide evidence about the *first* pass where colors diverge.
4. Compare the two PNGs locally:

```powershell
python -m pip install pillow
python .\tools\scarlet_compare_suyu.py `
  --suyu "D:\captures\suyu.png" `
  --native "D:\captures\native.png" `
  --diff "D:\captures\scarlet-rgb-difference.png"
```

The comparison refuses images of different resolutions instead of rescaling
away a real texture or aliasing error. It reports per-channel RGB error, an
aggregate MAE, a PSNR and an amplified PNG difference. Identical input images
score MAE=0; **time misalignment, weather, TAA jitter, UI and moving objects
can dominate difference metrics**. A high score alone does not identify a
shader defect.

## Order of actual fixes after the comparison

1. **Unchanged game inputs**: the same ExeFS/NSO build, RomFS/update assets,
   and texture contents. Validate ASTC, BC7 sRGB, BC5 SNORM and swizzles
   against sampled GPU images, not just CPU texture bytes.
2. **Lighting/materials**: compare native G-buffer RT0–RT4, stencil material
   IDs, normal/tangent directions, blending, alpha and shader interpolation
   against the suyu frame.
3. **Color and cinematic effects**: verify the first divergent frame pass,
   especially the log-space 32-cubed LUT, sRGB views, exposure, bloom,
   tone mapping, stencil and DOF. Do not use a neutral LUT as a 'fix'.
4. **Presentation only after scene fidelity**: re-enable SMAA/upscaling/FSR,
   making sure source depth, motion vectors, jitter and color space match the
   reference. Then optimize performance without changing output.

No original suyu screenshots or patched suyu renderer source are bundled in
this branch, so exact parity cannot be verified by the GitHub CI alone.
