# d3d12_x

## add Unknown function info

- `WaitFrameEventX` (device slot 80) takes `(type, timeout, objects, flags, token*)`; the objects and flags are logged and ignored, and the token's format is chosen, not observed.
  Title IDs: 1738CA2A

## add Functional

- Textures created in a copy state start in the common state on the PC, so copies on any queue promote them. A title barrier that names the copy state as its before state then disagrees with the PC's tracking after a copy queue has let the texture decay.
  Title IDs: 1738CA2A
- Textures answer `GetGPUVirtualAddress` with CPU memory of their own; written pages send the whole texture to the GPU before the next `ExecuteCommandLists`, on the queue that submits. The copy relies on implicit state promotion, is not ordered against other queues, and GPU writes to the texture never reach the CPU memory.
  Closes when: titles are seen reading GPU-written textures through the CPU, or the copy order across queues matters.
  Title IDs: 1738CA2A
- Only single-sample 2D textures of the formats `xg_x` describes get CPU memory; other textures still answer `GetGPUVirtualAddress` with 0.
  Title IDs: 1738CA2A
- Textures are handed to the title as PC objects whose function table is replaced by a copy with its own `Release` and `GetGPUVirtualAddress`.
  Closes when: resources are wrapped in console objects.
