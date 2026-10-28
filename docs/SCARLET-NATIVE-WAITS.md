# Native Vulkan wait correction

The user captures show 14–17 FPS with no shader compilation and large cumulative
timeline waits. They do not establish whether the GPU workload, host scheduling,
or resource reuse is the primary limiter. The reference graphics switch leaves
the blue sky error and exposes incorrect hair lighting; retain normal hair
compatibility. This change does not claim a colour fix or measured FPS gain.

Completed timeline values are now published to an atomic monotonic watermark.
A later wait at or below a confirmed completion returns without another driver
wait. It never skips a submission above that watermark; successful waits and
counter queries are the only publishers. Backend initialization resets it.
Timeout now fails before resource reuse, instead of merely logging and proceeding.

With LARECOMP_VK_PROF (runner -Profile), [vk-wait] records real driver wait count
and duration per source: command slots, transient pressure, readback, heap import,
frame slots, present copies, guest fences, finish and TF diagnostics. It measures
host wait duration, not GPU execution time. Counters are off without profiling.
Normal frame statistics retain their existing accounting. This does not remove
barriers or change guest ordering, visual corrections, resource ownership or AOT.

Validation: tools/test_vulkan_completion.py exercises concurrent out-of-order
completion publication, lower-value rejection, future-value exclusion and reset
against the actual production header. The Windows CI build checks integration.

For an opt-in measurement, launch the normal native runner with -Profile and
without -GraphicsReference, then collect its console log and
D:\ScarletNativeCache\native-build\profile\frames.csv. A same-scene comparison
with warm caches on the user's Radeon is still required. The reported svcBreak
is a separate unresolved guest/runtime correctness issue.
