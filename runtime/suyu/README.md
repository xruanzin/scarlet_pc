# suyu runtime foundation

Scarlet PC currently uses **suyu v14.1.0** as the GPL-3.0 runtime/recompiler foundation for its Windows x86-64 AOT path.

Validated Scarlet configuration:

- Title ID: `0100A3D008C5C000`
- Host: Windows x86-64
- CPU path: ArmRecomp / Hybrid AOT
- Shared AOT modules:
  - `recompiled_rtld.dll`
  - `recompiled_image.dll`
  - `recompiled_subsdk0.dll`
  - `recompiled_sdk.dll`
- Validated suyu revision: `347fcf91d95cd39cb5c3db50f1b6d827ad699311`

The validated package uses a small `suyu-cmd.exe`-derived host renamed locally to `ScarletPC.exe`, with the game code kept in shared AOT DLLs instead of one oversized PE executable.

The suyu runtime itself is not original Scarlet PC code. Scarlet PC owns and maintains its Scarlet-specific integration, automation, diagnostics, patches and performance work.

No game files, console keys, firmware, Nintendo assets or generated game-derived AOT binaries belong in this repository.

Upstream: https://github.com/suyu-emu/suyu-main
