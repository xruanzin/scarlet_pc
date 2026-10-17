# Pokémon Scarlet bootstrap

This branch is the first Scarlet-specific development layer on top of the Let's Go Recomp runtime.

## Important

Do **not** put Pokémon Scarlet game files, Switch keys, generated game code or Nintendo assets in this repository.
Keep them only on the development machine. The existing `.gitignore` excludes the common game-file paths and formats.

## First diagnostic

Run the ExeFS triage tool against the extracted game files before changing the loader or generator module list:

```powershell
python tools\scarlet_triage.py "D:\Games\Scarlet\Base\exefs" --json scarlet_base.json
```

If you also have an extracted update ExeFS, analyze both in one run:

```powershell
python tools\scarlet_triage.py `
  "D:\Games\Scarlet\Base\exefs" `
  "D:\Games\Scarlet\Update\exefs" `
  --json scarlet_triage.json
```

The script finds every NSO by its `NSO0` header instead of assuming the Let's Go module list. It reports:

- program ID from `main.npdm`;
- NSO filenames and sizes;
- Build IDs and SHA-256 hashes;
- text/rodata/data/BSS sizes;
- imports/exports and `DT_NEEDED`;
- relocation counts;
- which modules differ between two supplied ExeFS sets.

The JSON/text report contains metadata only. It is the input for the next step: making the runtime loader and code generator use Scarlet's actual module set.

## Inherited assumptions still to replace

The fork currently still contains Let's Go-specific assumptions:

- Eevee/Pikachu program IDs in `runtime/gamesetup.c`;
- a fixed `main + sdk + subsdk0 + subsdk1` module list;
- Let's Go-specific launcher text and controls;
- build-address patches for Let's Go;
- an `aoc:u` service that currently reports zero DLC.

These will be replaced from the Scarlet triage data instead of hard-coding guesses about Scarlet's executable layout.
