# Pokémon Scarlet bootstrap

This branch is the first Scarlet-specific development layer on top of the Let's Go Recomp runtime.

For the SDK initialization failure `no function at 700657c59c`, see
[the diagnosis, static resume fix and Windows rebuild commands](SCARLET-SDK-INDIRECT-BOOT.md).

For the subsequent `malloc` access violation at `0x20`, see
[the writable-thread-pointer fix and runtime-only rebuild](SCARLET-THREAD-TLS-BOOT.md).

For the later NVN initialization stop at `0x7000a7db34`, see
[main-module indirect resumes and the main-only regeneration](SCARLET-SDK-INDIRECT-BOOT.md#later-boot-main-switch-after-nvn-initialization).

If the main rebuild finishes compiling but reports `clang: Argument list too long`,
see [the link recovery; no module regeneration is needed](SCARLET-SDK-INDIRECT-BOOT.md#link-recovery-after-argument-list-too-long).

For the later access violation after `[nvn] window with 2 textures`, see
[the confirmed 64-byte sampler-builder overflow and runtime-only fix](SCARLET-GRAPHICS-CRASH.md).

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

