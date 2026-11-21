# acphal

## change `AcpHalAllocateShapeContexts` takes two pointers

- The APU address field of the PCM array holds a byte address; the console writes the XMA array's field plus the XMA context count there.

## add Unknown function info

- `ApuAlloc` flags (1 and 2 observed) are logged, not decoded.
  Title IDs: 765B6743, 1738CA2A
- `ApuMapVirtualAddress` returns 0 for an address outside every APU block; what the console returns is unknown.

## remove `AcpHalAllocateShapeContexts` fails with `E_FAIL`

## add Functional

- `ApuCreateHeap` records the two heap sizes; `ApuAlloc` serves each block on demand below 4 GiB and is not limited by them.
  Title IDs: 765B6743
- EQ/compressor, filter/volume and PCM contexts are allocated, but nothing processes them.
  Closes when: their fields are known and processed.
  Title IDs: 64B02253
- `AcpHalCreate` fails with `E_NOTIMPL`: there is no audio control processor, so titles that need it run without audio.
  Title IDs: 1738CA2A
