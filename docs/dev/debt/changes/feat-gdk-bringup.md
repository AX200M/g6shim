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

## add Functional

- Console-only slots that are not implemented are probes: they log their first four calls and return `E_NOTIMPL`.
- `CheckFeatureSupport` fills the PC part of the 0x40-byte console `D3D12_OPTIONS` structure and zeroes the last 4 bytes.
  Title IDs: 1738CA2A
