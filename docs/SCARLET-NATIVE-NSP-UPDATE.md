# Scarlet PC Native — NSP base + update 4.0.0 (experimental)

This branch adds a native Windows update preparation path. It does not use
the suyu executable and does not rebuild the existing ArmRecomp AOT DLLs.

## Rationale

- Original Scarlet base 1.0.0 main Build ID: ca44e7dd62e61a2a…
- Existing AOT for Scarlet 4.0.0 main Build ID: 421c5411b487eb4d…
- These two executable versions are not interchangeable.

Fuse-Nx supports direct base package streaming, but does not currently handle
BKTR-encrypted update partitions. For update launches the runner uses hactool
with its --basenca feature, temporarily extracts base/update NCA containers,
and caches the patched ExeFS and RomFS image.

## Windows usage

First check git status --short --branch; preserve all local changes and work
from branch native/nsp-update-aot-20261027. With legally obtained keys and
game/update dumps:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime native -GamePath "D:\Pokemon Scarlet\Pokemon Scarlet.nsp" -UpdatePath "D:\Pokemon Scarlet\Pokemon Scarlet v4.0.0.nsp" -KeysPath "D:\Pokemon Scarlet\prod.keys" -AotManifestPath "D:\scarlet\Pokémon Scarlet - Hybrid AOT + JIT\aot_cache\aot_manifest.json" -Profile
~~~

KeysPath and AotManifestPath are optional when discoverable from the known
keys locations and the imported AOT export root. HactoolPath may be passed
if hactool.exe is installed outside the expected paths. The update preparer
automatically discovers local title.keys (including some emulator key folders).
For NSPs with common tickets (*.tik), it also extracts their encrypted titlekeys
locally and supplies both base and update Rights IDs to hactool through a
temporary HOME/.switch/title.keys file; this file is deleted after each run.

If the NSP has no common ticket, or contains a personalized ticket, the
encrypted titlekeys cannot always be derived automatically. In this case
provide a title.keys file obtained from the user's own console with
-TitleKeysPath, for example:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime native -GamePath "D:\Pokemon Scarlet\Pokemon Scarlet.nsp" -UpdatePath "D:\Pokemon Scarlet\Pokemon Scarlet v4.0.0.nsp" -TitleKeysPath "D:\Pokemon Scarlet\title.keys" -Profile -NoLauncher
~~~

Never paste any key files or unredacted hactool logs in public. All hactool
invocations in this preparer use --suppresskeys and --disablekeywarns; older
logs may still contain key values. Tests in tools/test_native_titlekeys.ps1
use only synthetic fixtures, not Nintendo keys or game assets.

The first run can use significant disk space and time: it extracts two sets
of NCAs and writes an updated RomFS image. **On Windows with a D: drive,
the default cache is now D:\ScarletNativeCache\scarlet-<identity-tag>.**
On machines without D:, the cache stays under build/native-update-cache.
Existing incomplete .work caches on C: are copied safely to D: (the original
is kept until the user chooses to delete it) to avoid repeating extraction.
D: must have enough space for the cache plus 8 GiB of working headroom.
-UpdateCacheRoot explicitly overrides the default, and no automatic
migration occurs for explicit override paths. This is not yet zero-extraction
BKTR mounting.

## hactool "Failed to write file!" (Windows)

This means hactool's fwrite() failed while saving an extracted section/file;
it is not proof that keys or NCAs are invalid. First check available disk
space on the volume that contains the native update cache (usually C:):

~~~powershell
Get-PSDrive -PSProvider FileSystem |
  Select-Object Name,@{N='FreeGiB';E={[math]::Round($_.Free/1GB,2)}}
~~~

The new preparer prints available space automatically when this specific
error occurs. It also reports the last file hactool tried to write. If C:
is short on free space but D: has ample space, the default runner now copies
the already-extracted NCA cache to D: automatically. Normally no manual move
is necessary. To choose a specific alternate output root, preserve the source
cache and supply -UpdateCacheRoot explicitly. For the previous manual approach:

~~~powershell
$old = "C:\Users\ruana\Downloads\scarlet_pc-update-test\build\native-update-cache\scarlet-8df208305cfada13"
$new = "D:\ScarletNativeCache\scarlet-8df208305cfada13"
New-Item -ItemType Directory -Force -Path (Split-Path $new -Parent) | Out-Null
Move-Item -LiteralPath ($old + ".work") -Destination ($new + ".work")
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\scarlet_native_run.ps1 -Runtime native -GamePath "D:\Pokemon Scarlet\Pokemon Scarlet.nsp" -UpdatePath "D:\Pokemon Scarlet\Pokemon Scarlet v4.0.0.nsp" -UpdateCacheRoot $new -Profile -NoLauncher
~~~

Only relocate after verifying the destination has sufficient free space; the
move can be large. Do not delete existing .work, force a new extraction, or
rebuild AOT DLLs. If hactool continues failing with ample free space, inspect
the new redacted diagnostic for the last written file and check filesystem
permissions/drive errors; an alternate cache volume is not a guarantee.


## Implementation

1. Prepare tool finds the base Program NCA by main.npdm Title ID.
2. It finds the updated Program NCA by the Scarlet 4.0.0 main NSO Build ID.
3. Hactool applies --basenca to reconstruct updated ExeFS and patched RomFS.
4. Preflight validates the Title ID and main Build ID. When available, the
   AOT source manifest supplies expected SDK/subsdk0 Build IDs too.
5. The native runner supplies the prepared ExeFS and RomFS image via
   LARECOMP_EXEFS_DIR and LARECOMP_ROMFS_IMAGE to ScarletPC.exe.
6. There is no implicit fallback to suyu for base + update alone.

The existing explicit -DlcPath legacy route remains until native DLC
overlays are implemented. This feature was not tested against the user's
real game or AOT DLLs in the GitHub-only environment. No promise of a
successful boot or gameplay performance is implied.

Run tools/test_native_runner.ps1 on Windows for a mocked runner regression
test, then inspect the logs of a real -Runtime native launch. On failure,
preserve the build/native-update-cache/*.hactool.log and the native runtime log.
Do not bypass Build ID checks to make an incompatible 1.0.0 ExeFS launch.
