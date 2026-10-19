# Attachment formats, cinematic focus and material diagnostics

The Vulkan pipeline and render-target view now share `vk_attachment_format`.
Previously an sRGB view could be paired with a pipeline declaring UNORM.
Explicit guest views and backend fallback formats remain authoritative;
`LARECOMP_VK_RAW_SRGB_ATTACH` changes both declarations consistently.

`LARECOMP_NO_DOF=1` opts out of Scarlet's recognized depth-of-field composite
(fingerprint `fbff97881854688e`), sampling its sharp scene input t2 instead.
The default is unchanged. This does not disable upstream blur passes or imply
a performance improvement. The statue capture has sharp subject / blurred
foreground consistent with cinematic focus; it does not alone prove a depth bug.
Other postprocessing and UI still execute.

Hair cyan/purple emission is not confirmed fixed. Prior F9 captures saved only
RT0, missing geometry passes with RT0 unbound and material outputs in RT1–4.
F9 now saves up to 32 additional material attachment snapshots after geometry
draws and logs their formats. Collection uses the capture start timestamp so
slow readback does not discard early images. These readbacks occur only on F9.

Validation: native attachment-view tests, generated DOF GLSL/SPIR-V tests,
layer-varying regression, PowerShell collection fixture with a 30-second capture,
and compilation of the modified Vulkan units. No Windows game/GPU validation
was possible in this environment. Capture F9 while affected hair is visible and
collect a fresh ZIP to distinguish material, lighting and later bloom errors.
