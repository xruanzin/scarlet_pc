# Pokémon Scarlet base-game executable baseline

Captured from the user's legally extracted base-game ExeFS with
`tools/scarlet_triage.py`.

## Program

- Program ID: `0100A3D008C5C000`
- ExeFS modules used by the recomp bootstrap: `main`, `sdk`, `subsdk0`
- `rtld` is present in ExeFS but is represented by the host loader/runtime rather
  than statically recompiled as a game-code module.

## Modules

| Module | Build ID | Image size | Imports | Exports |
| --- | --- | ---: | ---: | ---: |
| main | `ca44e7dd62e61a2a4e8889b3319ae173375770fd` | 71,258,112 | 955 | 44 |
| sdk | `3644a6760469451cabc99f00c540acc2a64aee41` | 14,315,520 | 13 | 27,161 |
| subsdk0 | `d0f13015e5badf2243d60a2bc9b978dddba80608` | 6,922,240 | 659 | 10,882 |

The base `main` declares `multimedia.nss` and `nnSdk.nss` as needed
libraries.

## Guest address layout

The inherited Let's Go addresses cannot be reused unchanged because Scarlet's
`main` image is 71,258,112 bytes (0x43f5000), which would overlap the old SDK
base at +64 MiB.

The Scarlet bootstrap therefore uses:

- `main`: `0x7000000000`
- `sdk`: `0x7006000000`
- `subsdk0`: `0x7008000000`

These bases must stay identical in the generator, game-library builder and
runtime loader.

## Address-specific patches

No Let's Go address-specific gameplay patches or hooks are enabled for Scarlet.
They must be rediscovered against Scarlet builds before being enabled.
