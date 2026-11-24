# xg_x

## Build

None.

## Silent stubs

None.

## Unknown function info

- `XGComputeOptimalTileMode` answers from the dimension and sample count only: tile mode 13 for single-sample 2D textures, linear general (8) otherwise.
  Closes when: a title is seen getting another tile mode from the console.
  Title IDs: 1738CA2A
- Texture computer slots after `GetResourceLayout` (2) are not identified; they are probes that log and return `E_NOTIMPL`.
  Title IDs: 1738CA2A

## Functional

- Only tile modes 13 and 8 are described; computers for other tile modes get `E_INVALIDARG`. Mode 13 matches the console's layouts, mode 8 uses the row pitch (256 bytes) and placement (512 bytes) of PC copy footprints.
  Closes when: a title asks for another tile mode.
  Title IDs: 1738CA2A
- Only single-plane textures of DXGI formats are described; planar, console-only and 1-bit formats get `E_INVALIDARG`.
  Title IDs: 1738CA2A
- Without a `console_xg_x.dll` next to it, `XGComputeOptimalDepthStencilTileModes`, `XGComputeTexture2DLayout` and `XGCreateTextureComputer` log and return `E_NOTIMPL`. Forwarded to the console library, they pass at most eight integer arguments.
  Closes when: g6shim implements them, or a title is seen passing more arguments.
