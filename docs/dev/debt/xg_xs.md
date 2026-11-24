# xg_xs

## Build

None.

## Silent stubs

None.

## Unknown function info

- `XGSuggestSwizzleMode` ignores its flags argument.
- `XGCreateTextureComputer`'s copy-out slot ignores its sample index.
- A tail mip of the 4KB `R_X` and 64KB `R_X` modes reports swizzle mode 3, which the GPU does not otherwise use.

## Functional

- `XGSuggestSwizzleMode` answers from the older optimal-mode rule: linear for 1D textures, 64 KiB `Z_X` for depth-stencil, 64 KiB `R_X` for render targets, 4 KiB `S` otherwise; size, samples and the 2D/3D/cube distinction are ignored.
  Closes when: streamed textures are seen scrambled, or the newer rule is observed.
- Texture addressing is implemented for linear and for the single-sample modes without pipe or bank XOR; element offsets and copies of XOR (`_T`, `_X`) and multisampled layouts fail with `E_NOTIMPL`/`E_INVALIDARG`.
  Closes when: a title copies into or out of an XOR or multisampled layout.
- Textures carry no compression metadata planes (HTILE, CMASK, FMASK, DCC), so render targets and depth buffers are smaller than the console makes them.
  Closes when: a title sizes a pool from console sizes and runs out.
- 96-bit, 1-bit, packed 4:2:2 and video DXGI formats have no layout and get `E_INVALIDARG`.
  Closes when: a title asks for one of those formats.
- 64 KiB `Tiling` is implemented for 2D layouts only; 4 KiB modes get `E_FAIL` and 3D layouts `E_NOTIMPL`. With a mip tail, at most two slices are listed and entries for slices after the first stay zero.
- Texture computer slots 0x18, 0x20, 0x28, 0x38, 0x58 and 0x60 (size, alignment, mip offset, element coordinate, view descriptor and tiled-resource query) log and return `E_NOTIMPL`.
