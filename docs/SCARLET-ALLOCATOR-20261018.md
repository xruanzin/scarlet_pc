# Scarlet allocator reuse and late transform-feedback controls

## Evidence

The supplied `scarlet-diagnostics-20261018-181744-017.zip` contains completed HLE
intervals reporting, for example, 3811 ms across 3396 aligned StandardAllocator
allocations and 2134 ms across 3252 frees. Another interval reports 3969 ms
across 50428 allocations and 1959 ms across 53834 frees. These are inclusive
wall-time sums across threads, including lock waiting; they are not exclusive
CPU time or additive frame budgets.

The implementation scanned split free spans under a global mutex and allocated
new host metadata on every allocation/free. Alignment fragments can accumulate
in those lists. The profile identifies this as a concrete optimization target;
it does not identify every source of the low FPS.

## Change

Small allocations use exact size/alignment caches shared by all guest threads.
Capacity rounds up to 16 bytes while GetSizeOf still returns the requested
size. Classes cover capacities through 4096 bytes and alignments 16–4096.
Free keeps the allocation record with its span, so a hit performs no host
metadata allocation and no free-span search. Reallocate stays in place when
the new requested size fits the reserved capacity, preserving alignment/data.

The cache retains at most 2048 records per class and 64 MiB of guest spans.
Overflow and larger allocations use the existing general free lists. Reset
releases cached metadata. Guest addresses, commit-before-return behavior and
the global synchronization lock remain in place. This is not a lock-free
allocator or a solution to general free-span coalescing. Internal rounding
adds at most 15 bytes per live allocation; retained cache spans may temporarily
be unavailable to a different size/alignment class.

## Graphics capture correction

The old collector exhausted its 128-control quota on early programs. The seven
no-position vertex shader addresses in this run have no matching controls in
the ZIP. The TF bind/begin/end calls are present, but that does not reveal the
compiler's stream-output attribute mapping. TF remains unimplemented.

Controls copied at SetShaders now survive until late TF selection independently
of the initial quota. The collector reserves 128 TF program entries and logs
their code addresses. Snapshots are freed on program replacement/finalization,
with a 16 MiB live snapshot budget. This fixes collection, not rendering. It
does not guess the TF output layout or read beyond the known 0x704-byte prefix.

## Validation

`tools/test_std_allocator.py` exercises requested sizes, alignment, data across
reallocation, zero/overflow requests, reset, both cache caps, 100 rounds of
eight threads allocating and freeing each other's blocks, and 153600 mixed
small allocation/free pairs after warmup. The mixed workload creates zero new
host metadata records and does not advance the guest arena frontier. On the
pre-change source, the same workload creates 307200 metadata records and fails
the reuse assertion (negative control). Synthetic timing is not game FPS.

The diagnostic tests check late TF selection after the initial quota fills,
binding before/after Begin, pause/resume/end, immutable snapshots after caller
memory changes, independent caps and disabled collection. Edited translation
units compile on Linux. Actual Scarlet/Windows/AMD execution remains a local
validation step; neither graphics correctness nor an FPS gain is established.

Build and run using `scarlet_build_windows.ps1`, then
`scarlet_run.ps1 -Profile -GraphicsDiagnostics`. Stay in customization for
20–30 seconds and close normally. Compare the allocator HLE totals and frame
times and inspect the new `[tf-diag] TF program` lines. The launcher's ZIP
contains this run's controls, matching shader dumps, profile log and build hash.
