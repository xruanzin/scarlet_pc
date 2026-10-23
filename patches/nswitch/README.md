# Scarlet-specific Nswitch runtime patches

This directory is the home for Scarlet PC changes applied to the pinned third-party runtime foundation.

Current validated result:

- all four Scarlet AOT modules load;
- AOT-first execution is active;
- ABI 6 / fastmem / guard-v2 / FPX1 negotiation succeeds;
- dynamic fallback is available when uncovered code is reached;
- the game boots through the Scarlet PC runtime path.

Future Scarlet-specific runtime changes should be stored here as numbered patches, for example:

```text
0001-scarlet-aot-diagnostics.patch
0002-scarlet-aot-performance.patch
0003-scarlet-render-fixes.patch
```

Every patch should document the exact validated runtime revision and the Scarlet test result it was validated against.

Exact third-party provenance and licensing are kept under `licenses/third-party/` and `THIRD_PARTY.md`.
