# xmem

## Build

None.

## Silent stubs

None.

## Unknown function info

- `XMemAllocatePhysicalPages` numbers 64 KiB pages in 4 KiB frames: a page's number is a multiple of 16, and titles keep it as 21-bit `number >> 4`.
  Closes when: a title is seen keeping the numbers in another form.
- The `XMemAlloc` memory-type bits (29-32) and the other attribute bits are not decoded; only the alignment field (24-28) and the 32-bit address bit (33) are.
  Closes when: a title needs a memory type.

## Functional

- `XMemVirtualAlloc` turns a 64 KiB-page reservation into a host placeholder; committing part of such a reservation later is not supported, because only a whole placeholder can be replaced.
  Closes when: a title commits part of a 64 KiB-page reservation.
- `XMemVirtualQuery` reports graphics memory (flags 3) and no tag for every committed page; the flags and tag a region was allocated with are not remembered.
  Closes when: a title tells CPU-only memory apart by these fields.
- `XMemAllocDefault` blocks come from the host heap above 2^40, unlike the console's; nothing sub-allocates them from a reservation below 2^40.
  Closes when: a title builds ray-tracing structures in this memory.
- `XMemGetWorkingSetStatistics` fills every field whatever `which` asks for, does not split use by bandwidth class, and reports GPU-optimal use as 0.
  Closes when: a title budgets from `gpu_optimal_used`.
- Title memory defaults to the high-memory configuration (10 GiB GPU-optimal, 3.5 GiB standard); nothing selects the small one (7,776 MiB standard).
  Closes when: the launcher selects a console configuration.
- `XMemTransferMemory` moves only title memory to the system (kind 0); tools transfers return 0, and `XMemIsToolsMemAvailable` reports none, because a retail console has no tools memory.
- A commit-limit failure logs the request and the system commit; it does not write a crash report.
  Closes when: the crash reporter takes the report.
