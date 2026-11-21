# acphal

## Build

None.

## Silent stubs

None.

## Unknown function info

- `AcpHalAllocateShapeContexts` takes two pointers whose layout is unknown.

## Functional

- `AcpHalAllocateShapeContexts` fails with `E_FAIL`: there is no hardware audio, and audio libraries fall back to software mixing.
  Title IDs: 1738CA2A
