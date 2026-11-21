# d3d12_x

## add Silent stubs

- `D3D12XboxSetProcessDebugFlags` stores the flags; nothing reads them.
  Title IDs: 1738CA2A

## add Unknown function info

- `D3D12XboxCreateDevice` logs its parameter block without decoding it beyond the version, and ignores the console adapter argument.
  Title IDs: 1738CA2A
- `CreateCommandQueueX` ignores the flags, engine and queue index; DMA queues become PC copy queues.
  Title IDs: 1738CA2A
- Descriptor heap `GetDesc` (slot 8) is taken to return through a hidden pointer passed before the heap; no title has called it yet.
- Device slot 69 takes a 32-bit id and a value and is not identified; it is a probe.
  Title IDs: 1738CA2A
- Device slot 68 fills a structure through its only argument and is not identified; it is a probe and leaves the structure as it was.
  Title IDs: 1738CA2A
- `SetFrameIntervalX` ignores its output and flags; `ScheduleFrameEventX` honours flag 1 (no signal) and ignores the others.
  Title IDs: 1738CA2A
- Graphics command list slots 56 to 59 are taken to be `SetMarker`, `BeginEvent`, `EndEvent` and `ExecuteIndirect` in PC order; no title has called them yet. Slots 60 and up are probes.
  Title IDs: 1738CA2A
- DMA command list slots 16 to 21 are taken to be `BeginQuery`, `EndQuery`, `ResolveQueryData`, `SetMarker`, `BeginEvent` and `EndEvent`; no title has called them yet. Slots 22 and up are probes.
  Title IDs: 1738CA2A
- Command list `ResourceBarrier` is taken to return a `UINT`; g6shim returns 0 without knowing what the title reads from it.
  Title IDs: 1738CA2A

## add Functional

- Console-only slots that are not implemented are probes: they log their first four calls and return `E_NOTIMPL`.
- `CheckFeatureSupport` fills the PC part of the 0x40-byte console `D3D12_OPTIONS` structure and zeroes the last 4 bytes.
  Title IDs: 1738CA2A
- Frame events come from a timer that starts at `SetFrameIntervalX`, not from the display's vertical blank.
  Title IDs: 1738CA2A
- Shader-visible sampler heaps of the console's unbounded sampler type (4) become PC sampler heaps of at most 2048 descriptors; `GetDesc` reports the requested size.
  Closes when: a title is seen using more than 2048 samplers, or the heap is split.
  Title IDs: 1738CA2A
- Console DMA command lists are PC copy lists; their console-only operations are probes.
  Title IDs: 1738CA2A
- DMA command list transitions to or from states other than the common and copy states are dropped; the PC copy queue promotes resources from the common state and lets them decay back after each execution.
  Closes when: a title shows a dropped transition that the PC queue does not cover.
  Title IDs: 1738CA2A
