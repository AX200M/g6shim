# xg_x

## Build

None.

## Silent stubs

None.

## Unknown function info

- `XGComputeOptimalTileMode` ignores its arguments and returns the linear general tile mode (8).
  Title IDs: 1738CA2A
- Texture computer slots after `GetResourceLayout` (2) are not identified; they are probes that log and return `E_NOTIMPL`.
  Title IDs: 1738CA2A

## Functional

- Texture layouts are linear, with the row pitch (256 bytes) and placement (512 bytes) of PC copy footprints, not the console's tiled layouts.
  Closes when: a title is seen reading tiled data it wrote itself, or the GPU's tiled layout is needed.
  Title IDs: 1738CA2A
- Only single-plane textures of DXGI formats are described; planar, console-only and 1-bit formats get `E_INVALIDARG`.
  Title IDs: 1738CA2A
