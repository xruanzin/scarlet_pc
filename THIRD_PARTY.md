# Third-party foundations

Scarlet PC is a Pokémon Scarlet-specific Windows recompilation project created and maintained by **xruanzin**.

The project uses and modifies third-party open-source components where appropriate. Those components retain their own copyright notices and licenses.

## Scarlet PC original work

The Pokémon Scarlet-specific implementation in this repository is developed and maintained by **xruanzin**.

This includes the port-specific functionality and integration required to make Pokémon Scarlet run through the Scarlet PC pipeline, including:

- Pokémon Scarlet-specific AOT integration and module handling;
- Scarlet-specific runtime integration and compatibility work;
- build, packaging, setup and launch tooling created for this port;
- diagnostics and instrumentation added for this project;
- port-specific fixes and runtime patches;
- performance profiling and optimizations developed for Scarlet PC;
- CPU/AOT execution-path tuning and fallback integration;
- Windows packaging and the standalone Scarlet PC runtime workflow.

These Scarlet-specific functions, fixes and optimizations are Scarlet PC project work and are not claimed as part of the unmodified upstream runtime.

## suyu

Scarlet PC's runtime/recompiler foundation incorporates modified GPL-licensed work derived from **suyu**:

https://github.com/suyu-emu/suyu-main

The validated upstream baseline is suyu v14.1.0 / revision `347fcf91d95cd39cb5c3db50f1b6d827ad699311`.

suyu is licensed under GNU GPL v3 and documents its own upstream lineage, including Eden and yuzu.

Scarlet PC does not claim authorship of the original suyu runtime/recompiler. Credit to suyu applies to the third-party foundation from which portions of the runtime were derived.

The Pokémon Scarlet-specific port functionality, integration, compatibility changes, tooling and performance optimizations described above were implemented and maintained for Scarlet PC by **xruanzin**.

## Game content

Scarlet PC does not distribute Pokémon Scarlet, Nintendo Switch firmware, console keys, or generated game-derived binaries. Users must provide their own legally obtained game files and required console data.
