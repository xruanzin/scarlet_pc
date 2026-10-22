# Scarlet-specific suyu patches

This directory is the home for Scarlet PC changes against the pinned suyu runtime.

The initial bootstrap does not invent a runtime patch: suyu v14.1.0 already contains the Windows compatibility loader for `recompiled_*.dll` modules, and that path has been validated with Pokémon Scarlet.

Current validated result:

- all four Scarlet AOT modules load;
- ArmRecomp is active;
- ABI 6 / fastmem / guard-v2 / FPX1 negotiation succeeds;
- the game boots and renders normally;
- the observed startup run recorded zero recompilation gaps.

Future Scarlet-specific runtime changes should be stored here as numbered patches, for example:

```text
0001-scarlet-aot-diagnostics.patch
0002-scarlet-aot-performance.patch
0003-scarlet-render-fixes.patch
```

Every patch should document the exact upstream suyu revision and the Scarlet test result it was validated against.
