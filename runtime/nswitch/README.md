# Scarlet PC runtime boundaries

Scarlet PC exposes two project-owned conceptual layers:

    ScarletPC.exe
          ↓
    Scarlet::GameRuntime
          ↓
    Nswitch::Runtime
          ↓
    AOT / Vulkan / HLE / dynamic fallback

Scarlet::GameRuntime owns Pokémon Scarlet-specific validation, preparation and launch policy.

Nswitch::Runtime is the stable runtime boundary used by Scarlet PC. It intentionally hides upstream command-line/frontend details from the launcher.

The current runtime host is still built as one executable internally. The planned physical split into nswitch-runtime.dll, nswitch-video.dll and nswitch-audio.dll will happen only when those components have real independent ABI boundaries; placeholder DLLs are not shipped.

Third-party provenance and licensing remain documented in THIRD_PARTY.md.
