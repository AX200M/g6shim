# d3d12_x

## add Unknown function info

- `WaitFrameEventX` (device slot 80) takes `(type, timeout, objects, flags, token*)`; the objects and flags are logged and ignored, and the token's format is chosen, not observed.
  Title IDs: 1738CA2A
- `FlushPipelineX` (graphics list slot 67) is taken to be a cache flush over a memory range and does nothing; its flags are not decoded.
  Title IDs: 1738CA2A
- `PresentX` ignores its parameters and the pipeline token.
  Title IDs: 1738CA2A
- The device's out-of-line flags (`+0x18`) are all set; which inline functions each bit covers is not known.
  Title IDs: 1738CA2A

## add Functional

- Textures created in a copy state start in the common state on the PC, so copies on any queue promote them. A title barrier that names the copy state as its before state then disagrees with the PC's tracking after a copy queue has let the texture decay.
  Title IDs: 1738CA2A
- Textures answer `GetGPUVirtualAddress` with CPU memory of their own; written pages send the whole texture to the GPU before the next `ExecuteCommandLists`, on the queue that submits. The copy relies on implicit state promotion, is not ordered against other queues, and GPU writes to the texture never reach the CPU memory.
  Closes when: titles are seen reading GPU-written textures through the CPU, or the copy order across queues matters.
  Title IDs: 1738CA2A
- Only single-sample 2D textures of the formats `xg_x` describes get CPU memory. Without the console's XG library they are laid out in tile mode 13, so data a title writes in another layout comes out scrambled; other textures still answer `GetGPUVirtualAddress` with 0.
  Title IDs: 1738CA2A
- Textures are handed to the title as PC objects whose function table is replaced by a copy with its own `Release` and `GetGPUVirtualAddress`.
  Closes when: resources are wrapped in console objects.
- Texture transitions lose the vertex and constant buffer and index buffer states; a transition left with equal states is dropped.
  Title IDs: 1738CA2A
- `PresentX` copies the first resource of the first plane into a flip-model swap chain on the title's largest top-level window and presents without waiting for the display; other planes are ignored.
  Title IDs: 1738CA2A
- Constant buffer views that break the PC's 256-byte alignment read a copy of their data, refreshed from the title's upload buffer before every `ExecuteCommandLists`, so frames still on the GPU may see constants written for a later frame. Views outside upload buffers move down to the alignment and read the wrong bytes.
  Closes when: the copies are tied to the submissions that use them.
  Title IDs: 1738CA2A

## change DMA command list transitions

- DMA command list transitions of textures, and transitions of buffers to or from states other than the common and copy states, are dropped; the PC copy queue copies into textures in the common layout, promotes resources from the common state and lets them decay back after each execution.
  Closes when: a title shows a dropped transition that the PC queue does not cover.
  Title IDs: 1738CA2A

# xg_x

## change returns the linear general tile mode

- `XGComputeOptimalTileMode` answers from the dimension and sample count only: tile mode 13 for single-sample 2D textures, linear general (8) otherwise.
  Closes when: a title is seen getting another tile mode from the console.
  Title IDs: 1738CA2A

## change Texture layouts are linear

- Only tile modes 13 and 8 are described; computers for other tile modes get `E_INVALIDARG`. Mode 13 matches the console's layouts, mode 8 uses the row pitch (256 bytes) and placement (512 bytes) of PC copy footprints.
  Closes when: a title asks for another tile mode.
  Title IDs: 1738CA2A

# xg_x

## add Functional

- Without a `console_xg_x.dll` next to it, `XGComputeOptimalDepthStencilTileModes`, `XGComputeTexture2DLayout` and `XGCreateTextureComputer` log and return `E_NOTIMPL`. Forwarded to the console library, they pass at most eight integer arguments.
  Closes when: g6shim implements them, or a title is seen passing more arguments.
