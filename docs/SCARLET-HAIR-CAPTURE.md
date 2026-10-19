# Hair colour investigation on native Vulkan 1.3

The DirectX/Dozen path remains available. The PowerShell runner defaults to native
Vulkan 1.3, including when a newer loader or GPU advertises Vulkan 1.4.

The isotropic workaround did not resolve the reported blue/purple hair. The
academy MRT capture has neutral hair base colour and the first lighting output
already has the defect, before bloom/tone mapping. The later stencil capture
clips the head. These observations do not establish a root cause or prove that
the hair was assigned the wrong material ID. Further global gamma, tint or
specular changes are not justified by those captures.

A RenderDoc capture preserves the frame's GPU resources, descriptor bindings,
constant buffers, shader modules and pipeline state. It can distinguish wrong
input data from incorrect shader calculations. The existing tools/rdc_draw.py
and tools/rdc_skin.py provide replay inspection helpers, although replay still
requires a compatible GPU/driver. This update adds capture launch support; it
does not fix hair rendering.

## Capture

Install the Windows x64 RenderDoc distribution from https://renderdoc.org.
Run the usual build after pulling, then:

```powershell
cd "C:\Users\ruana\Downloads\scarlet_pc"
Remove-Item Env:LARECOMP_HAIR_ISOTROPIC -ErrorAction SilentlyContinue
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_run.ps1 -Backend Vulkan -RenderDoc
```

If installed elsewhere, pass `-RenderDocCommand "D:\RenderDoc\renderdoccmd.exe"`.
The runner prints a unique capture directory under build/rdc/, sets the working
and output directories explicitly, and launches the EXE through renderdoccmd.
It clears inherited automatic frame-number capture settings for this launch and
restores them afterwards. Runtime settings, game arguments and backend selection
still go through the normal runner. Missing RenderDoc produces a concrete error.

When affected hair is clearly visible in the classroom, press F12 once, wait
for capture to complete, then close the game. Send the .rdc from the printed
directory, the matching scarlet_boot.log and a screenshot of the visible defect.
Keep game-derived captures out of the public source repository.

Inspection sequence: identify the hair geometry and lighting draw, inspect its
actual bound constant buffers/textures, verify material ID/normal/tangent data,
and compare the shader result with those same inputs. Only a reproducible
arithmetic, binding or format mismatch justifies a permanent renderer patch.

References:
https://github.com/baldurk/renderdoc/blob/v1.x/docs/getting_started/quick_start.rst
https://github.com/baldurk/renderdoc/blob/v1.x/renderdoccmd/renderdoccmd.cpp

Validation: PowerShell parsing, native backend version/environment tests and
version-selection tests. No RenderDoc/Windows capture or GPU replay was run here.
