# List snapshots and immediate queue submission

The supplied `scarlet_boot(20261019-041526).log` stops during initial rendering:
`vkEndCommandBuffer(s->upload)` returns `-13` at the two-list batch finalizer.
This reports the failing API, not the underlying AMD driver cause. The four
screenshots are from other executions: the character and UI render, with face
corruption and a noisy later scene. They do not establish the last log's frame
or prove that one bug explains all remaining graphical defects.

## Changes

- List-end submission immediately drains the queue batch again. The previous
  two-list deferral is disabled, including an inherited `LARECOMP_QUEUE_BATCH=2`.
  Each list retains its own upload/main command buffers, existing ordering,
  timeline signal, resource retirement and Vulkan error checks. This is a
  conservative rollback of the path involved in the new crash, not a claim
  that the exact driver's error has been reproduced or diagnosed.
- Each `backend_execute` starts a new guest-copy cache epoch. Two execution
  calls can otherwise share a pending GPU timeline number while guest code
  changes a buffer between them. The old cache could return the previous
  constant/vertex snapshot at the same address and size. Within one execution,
  unchanged ranges still share a snapshot. GPU retirement remains keyed to
  the timeline; it is separate from the CPU snapshot version.

Transform feedback, packed output attributes and texture-layout changes already
present in the branch are retained. This patch neither changes the ROM nor
guesses a texture block height from screenshots.

## Validation

`tools/test_vulkan_list_snapshots.py` compiles the actual cache, executor entry,
queue-limit and list-end submit functions with mocked GPU allocation/submission.
It changes one guest buffer over 1000 execution calls while holding the pending
timeline fixed. Every execution must read current bytes and reuse one snapshot
within that execution. Separate cases verify that list end physically submits
immediately, with and without an inherited batching variable. All three cases
fail against the pre-change source and pass after the change. This is a CPU
regression harness, not a Vulkan-driver test.

The edited executor translation unit compiles with the Vulkan/SDL/VMA headers
on Linux. Actual Scarlet, Windows and the AMD proprietary driver remain untested
here. Complete graphics correctness and a stable 30 FPS are not established.

Build with `tools/scarlet_build_windows.ps1`, then run
`tools/scarlet_run.ps1 -Profile`. First verify whether this startup failure is
gone, then compare the remaining face/scene corruption on the same screen.
