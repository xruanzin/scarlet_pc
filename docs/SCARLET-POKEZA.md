# PokeZ-ARecomp adaptations for Scarlet

Reference inspected: `Hexveil404/PokeZ-ARecomp`, branch `pokemon-za`, commit
`ee3ae4cbde7fab45593fcf69ece97f6b51575a01`. The user supplied archive is the older
`a47f1e10556302245d574087a12ffbbdf73acebc`; both were compared. Scarlet starting
point: `f78a83dba285934a9c1abc2139aa929a357819d4`.

## Implemented

| Reference | Scarlet adaptation | Reason and limits |
| --- | --- | --- |
| `runtime/nvn/vulkan/exec.c`, `rscale_dedupe` / `drv_dedupe` (original change `f511cc6f`) | Compare only populated scale rows and referenced storage ranges; materialize the full block on a miss | Avoid repeated zero-fill and comparison of 1–2 KB blocks per stage/draw. Scarlet retains its 65-row shader ABI; copied bytes are unchanged. Compute keeps its existing constants path. |
| `runtime/nvn/vulkan/exec.c`, program key memo (`496ec0e7`) | Exact consecutive program input comparison before hashing/table lookup | Still uses the same shader compilation workers and table on a miss. Includes compiler constant ranges and both ShaderKeys; reset invalidates the memo. Existing pipeline memo remains. |
| `runtime/nvn/tex_hash.h`, upload checks (`9ba196db`) | Eight-lane full hash, four-lane sampled hash, full coverage for textures <=64 KB; no unused sample calculation for already-dynamic textures | Independent chains reduce serial arithmetic dependencies. Keeps Scarlet's write-watch, periodic full checks and dynamic texture rules. These are runtime change detectors, not persistent shader/cache identifiers. |
| `runtime/kernel.c`, waitlist / condition variable / address arbiter | FIFO within priority, priority-aware address wake, CAS updates against guest atomics, finish mutex handoff after a condition signal wins the timeout race | Prevent starvation by newer equal-priority waiters and lost concurrent word updates. True timeout still returns without the mutex. Reboot cancellation remains cancellation. No new spin or always-on tracing. |

`SCARLET_LEAN_CONSTANTS=0` selects the original constant-copy path.
`LARECOMP_TEX_FASTHASH=0` selects the previous texture hashes. Both are decided once.
The AOT DLL ABI and all four existing AOT DLLs are unchanged.

## Compared but not transplanted wholesale

* Scarlet already has an ordered render worker (`vulkan/renderthread.c`), pinned
  command lists, fence tickets, descriptor batching/reuse, pipeline reuse,
  upload deduplication, TF dependency tracking and dynamic state caching. Replacing
  them wholesale would lose Scarlet-specific resource and synchronization fixes.
* PokeZ's second **Vulkan recorder** (`record.c`) owns command/descriptor pools,
  submits/presents and proxy descriptor handles. It requires migrating every
  recording path, including Scarlet FSR/DLSS/UI, and distinguishing queued vs
  actually submitted sequences. It has not been integrated by this patch.
* PokeZ's CPU/GPU frame overlap includes persistent storage snapshots, write-watch,
  heap-tail allocation and shader address remapping. Removing Scarlet's fence wait
  alone would let the guest rewrite buffers the GPU still uses. This coupled
  migration is not included here. The newer lazy heap import also depends on the
  buffer-device-address translator/storage path and is not copied in isolation.
* PokeZ disables its straight-line shader form on AMD due to reported device loss
  (`exec.c::vk_exec_init`). No AMD shader/pipeline optimization switch is forced on.
* `tools/gen.py` / `regpromote.py` use a different generated-code ABI. Their local
  register promotion and `musttail` transfers cannot be added inside existing ABI6
  DLL machine code by changing the host. They require a deliberate regeneration.
* Z-A addresses, patches, shadow/object-ID passes, SSGI/reflections, FSR2 integration
  and gameplay HLE are not interchangeable with Scarlet's game/shader data. Scarlet's
  existing temporal providers and material fixes remain in place.

## Validation and performance

`tools/test_vulkan_lean_constants.py` compares production copies against full-block
reference bytes for every size 1..2048 and scale-table length 0..64, plus changes
and submission epochs. `tools/test_vulkan_program_memo.py` tests input changes and
normal table fallback. `tools/test_vulkan_texture_hash.c` tests unaligned reads,
tails, boundary lengths and mutations; CI runs it with ASan/UBSan.
`tools/test_targeted_sync_wake.py` runs the actual kernel with concurrent guest
updates, a deterministic signal/timeout race, ownership, priority/FIFO, overflow,
selective event wake and cancellation tests. CI builds the Windows runtime too.

No Scarlet FPS improvement is claimed. The supplied GPU report measures 11.450 ms
between timestamps around frame 1700, while the nearby log reports 30 fps. Later
log intervals around frames 5202–5427 show 18–20 fps but are different workloads;
subtracting that GPU sample from their frame times would not isolate CPU cost.
The trailing svcBreak is not demonstrated fixed by these synchronization changes.

## Attribution

The named implementations and `tex_hash.h` are adapted/copied from PokeZ-ARecomp's
LAS-derived code, Copyright (c) 2026 Peiikai, under the MIT license. The full
permission notice is retained in this repository's `LICENSE`. No game assets or
recompiled proprietary game functions are included.
