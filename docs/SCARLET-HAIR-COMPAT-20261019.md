# Optional isotropic hair lighting

`LARECOMP_HAIR_ISOTROPIC=1` selects an approximate lighting variant for the
captured Scarlet deferred shader (hash `aa68a5939321c84a`). Default is off.

The academy capture shows neutral base colour in RT1, directional values in
RT4 at hair pixels, and already blue/purple hair in the first lighting output.
Subsequent LUT/bloom changes cannot repair this upstream result. The newer
frame 993 contains stencil bytes but clips the head; its source/snapshot bytes
match, so it cannot establish the hair material IDs in that frame.

This is a compatibility workaround, not a verified faithful anisotropic BRDF
fix. For material ID 3 only, it selects the existing ordinary isotropic material
path and clears the RT4-derived emission registers: that attachment contains a
tangent for this material, not radiance. Albedo, normal and subsequent scene
lighting remain inputs. Directional hair highlights are replaced by conventional
ones; other material IDs and shader fingerprints retain their original paths.
It does not promise to fix a separate scene-wide blue lighting defect.

The implementation is bounded to the known unsigned BFE extracting bits 3–5
from stencil, gated by the exact shader fingerprint and a distinct shader key.
It does not rewrite game assets or globally desaturate the framebuffer.

Validation: synthetic BFE translation with flag off/on and nonmatching source,
selector env/hash tests, GLSL compilation and SPIR-V validation of synthetic and
captured lighting variants, and executor compilation. Windows/AMD rendering
cannot be tested here. Remove the environment variable to restore native hair
lighting. No performance claim follows from compiler tests.
