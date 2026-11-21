# acphal

## add Unknown function info

- `ApuAlloc` flags (1 and 2 observed) are logged, not decoded.
  Title IDs: 765B6743, 1738CA2A
- `ApuMapVirtualAddress` returns 0 for an address outside every APU block; what the console returns is unknown.

## add Functional

- `ApuCreateHeap` records the two heap sizes; `ApuAlloc` serves each block on demand below 4 GiB and is not limited by them.
  Title IDs: 765B6743
