# Upscaler loader

Titles that upscale with the console build of the public upscaler API load `amd_fidelityfx_loader_xbox.dll` and call four of its five exports: `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch` and `ffxQuery` (`ffxConfigure` is the fifth). The functions, descriptor headers and descriptor layouts are the public C API of the upscaler SDK; the console build differs only in the backend id inside descriptor types.

g6shim's replacement implements the five exports by forwarding to the host build of the same API, `amd_fidelityfx_loader_dx12.dll`, which must sit next to the game together with `amd_fidelityfx_upscaler_dx12.dll`. Both are the signed binaries from the public SDK release (`Kits/FidelityFX/signedbin`).

## Descriptor types

A descriptor type is a 64-bit value: bits 24-31 name the backend (0 host D3D12, 1 Vulkan, 2 console), bits 16-23 the effect (1 upscaler) and the low bits the structure.

| Type | Structure | Translation |
|---|---|---|
| backend `0x02` | backend device: header, device pointer | the console form (`0x02000002`) and the host form (`0x00000002`) are both replaced by the host form carrying the host device |
| backend `0x03` | allocation callbacks | dropped; the host backend allocates its own resources |
| `0x00010000` | upscaler create: flags, maximum render and upscale size, message callback | copied |
| `0x0001000b` | upscaler create version | copied; a create that fails with it is retried without it |
| `0x00010001` | upscale dispatch | command list and resources replaced by host objects, chained extensions dropped |
| `0x00010006` | reactive mask dispatch | same as the upscale dispatch |
| `4`, `0x00010009` | version list and memory usage queries | the device pointer is replaced by the host device |

Any other create descriptor or dispatch type is dropped or refused and logged once by type.

## Resources

Each `FfxApiResource` the title passes names a title resource. The replacement takes its host texture, transitions it on the title list's host command list to `NON_PIXEL_SHADER_RESOURCE` (inputs) or `UNORDERED_ACCESS` (outputs), and passes that state. The description keeps the title's flags and usage but takes type, size, depth and mip count from the host texture; the format is the host format unless the host texture is typeless and the title named a typed format. A resource whose host texture cannot be put in the wanted state fails the dispatch if required (color, depth, motion vectors, output) and is passed as empty if optional (exposure, reactive, transparency).

The host upscaler sets its own descriptor heaps, root signature and pipeline on the command list, so the list's cached bindings are dropped after every dispatch.
