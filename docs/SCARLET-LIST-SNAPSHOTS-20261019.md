# Sixteen-list queue batching with bounded slot lifetime

The supplied Windows source identifies commit e2439f0, which enables sixteen
logical lists per physical queue submission. That source still has SUBS=8 and
QBATCH_MAX_CBS=16. A list can produce both upload and main command buffers.

The ninth list wraps to the first slot before any physical submission occurs.
Its sequence still matches the pending timeline value, so sub() does not reset
its already-ended command buffers. Ending one again is invalid. The provided
AMD log reports vkEndCommandBuffer(upload) returning -13. The CPU regression
reproduces that invalid lifecycle; actual AMD execution is still untested here.

## Final changes

- Default to sixteen lists, with LARECOMP_QUEUE_BATCH clamped to 1–16.
- Use 32 submission slots (two full batches), and capacity for 32 command-buffer
  entries (upload+main for every list). Assert the ring is larger than a batch.
- Check capacity for a whole upload/main pair before ending either buffer.
- Preserve one guest-copy snapshot epoch per executor entry, independently of
  the GPU resource-retirement timeline.
- Integrate the user's e2439f0 history, retaining its texture-layout fixes,
  vertex bounds and scoped TF barriers. The merge retains the snapshots fix.
- Do not add or change jogar.ps1.

Slots are initialized lazily and reset only after their old GPU sequence is
waited. More slots may retain more descriptor resources. No shader/GPU clock,
30-FPS guarantee or complete graphical correction is implied by the batch size.

## Validation

The production sub() regression simulates 4096 upload+main lists in 256 batches.
Every new list must acquire a fresh/reset slot; command-buffer capacity must
cover both entries. The e2439f0 source fails at ring reuse before submission;
the fixed source passes. Additional production cache/executor/list-end tests
check fresh guest bytes at a fixed pending timeline, reuse within one list,
sixteen-list default submission and the one-list override. Vulkan calls are
mocked; this is not an AMD driver execution test.

Build using scarlet_build_windows.ps1 and run scarlet_run.ps1 -Profile with
LARECOMP_QUEUE_BATCH=16. The queue/list counters should show grouped submission;
explicit flushes/presentation/synchronization can submit smaller batches.
