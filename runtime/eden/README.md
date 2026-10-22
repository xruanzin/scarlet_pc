# Eden-referenced runtime integration

Scarlet PC's independent Windows runtime path was shaped with Eden as a technical reference for runtime architecture, build layout and integration ideas.

Validated Scarlet configuration:

- Title ID: `0100A3D008C5C000`
- Host: Windows x86-64
- CPU path: AOT-first with internal dynamic fallback
- Shared AOT modules:
  - `recompiled_rtld.dll`
  - `recompiled_image.dll`
  - `recompiled_subsdk0.dll`
  - `recompiled_sdk.dll`
- Runtime baseline revision: `347fcf91d95cd39cb5c3db50f1b6d827ad699311`

The package builds its own host as `ScarletPC.exe`, loads the game-derived AOT modules locally, and keeps the fallback engine inside the same runtime instead of requiring a separately installed emulator package.

Scarlet PC owns and maintains its Pokémon Scarlet-specific integration, automation, diagnostics, packaging, compatibility patches and performance work.

No game files, console keys, firmware, Nintendo assets or generated game-derived AOT binaries belong in this repository.

Exact third-party provenance, upstream names and licenses are documented separately in `THIRD_PARTY.md`.
