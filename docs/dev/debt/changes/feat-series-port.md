# project

## add Functional

- The shader maps have nothing to count: no step writes `docs/progress/<TITLEID>.shaders.json`, so a title's shader map shows "no data yet".
  Closes when: a shader translation count is written for a title.

# acphal

## add Silent stubs

- `IAcpHal::submit` completes every command that carries an id at once, sets its notification word and queues the completion message; context updates and every SHAPE unit other than the XMA decoder is acknowledged but not carried out.
  Closes when: a title depends on those commands' effects.
- `IAcpHal::connect` and `IAcpHal::disconnect` only record state and queue messages; no hardware processing starts or stops.
  Closes when: the coprocessor's flowgraph commands are carried out.

## add Functional

- XMA contexts decode from a cold decoder state when enabled and the context's overlap buffer is ignored, so consecutive contexts of one voice can seam.
  Closes when: decoder state is carried through the overlap buffer.
- Coprocessor message timing uses one clock per object, started when it is created; audio frames are approximated at 375 per second.
  Closes when: the console's audio frame clock is available.
- The flowgraph command completes a flowgraph without creating or running it.
  Closes when: flowgraphs are built and mixed.

## add Unknown function info

- `ApuMapApuAddress` returns null for an address outside every APU block; what the console returns there is unknown.
- `ApuIsVirtualAddressValid` is treated as a byte-range test; its second argument may instead be an alignment.

## remove `AcpHalCreate` fails with `E_NOTIMPL`

# shader

## add Functional

- A tracked address whose high half adds anything to a root descriptor's takes it as the carry of its computed term, so offsets stay below 4 GB.
  Closes when: a title is seen addressing more than 4 GB past a root parameter.
- Copy propagation folds constants only through `s_mov_b32`, `s_movk_i32`, `s_mov_b64`, `s_brev_b32` and `s_not_b32`; a descriptor or address built from a constant made any other way is not traced.
  Closes when: a title builds a descriptor constant with another instruction.

# xcrdapi

## add Build

- BCPack texture decompression needs a constant table file: `G6SHIM_BCPACK_TABLES`, or `bcpack-tables.bin` next to `XCRDAPI.dll`; without it BCPack requests fail while zlib and plain requests run.

## add Unknown function info

- The queue description's last 8-byte field is logged, not interpreted.
- Factory slots 0x00, 0x18, 0x30, 0x38, 0x48 and 0x58-0x78, file slots 0x18-0x50, status slot 0x00 and queue slots 0x28, 0x38 and 0x48-0x58 return `E_NOTIMPL` and log their offset.

## add Functional

- Only the factory interface `62959D11-D45F-4884-A0A3-F7862EA243AE` is answered; other interface ids return `E_NOINTERFACE`.
- Requests with option bits 5-20 (a further transform) or bit 23 (a physical-page source) fail with `E_NOTIMPL`.
- BCPack streams are unpacked one lane step at a time on one core.
- BCPack BC2 streams are rejected.
- zlib streams are decoded with the repository's inflater, which requires a complete dynamic tree and a matching Adler-32 trailer.

# shader

## add Functional

- Translated shaders declare every output to depend on every input in `dx.viewIdState`; the exact dependence sets need a dependence pass.
  Closes when: a dependence pass derives the input-to-output sets from the program.
- A custom border colour in an immediate sampler decodes as transparent black; the custom colour table entry is ignored.
  Closes when: a title uses a custom border colour.

# project

## add Functional

- Only `G:` is mounted, on the folder of the executable. The console's persistent local storage drive and its `T:\` temporary drive have no mount, and nothing calls the free-letter mount helper yet.
  Closes when: local storage and temporary folders are given drives.
- The forwarders translate only the functions listed in `docs/abi/g6shim_file.md`. The other path-taking exports of the replaced DLLs stay plain forwarders: in the file sets `CreateDirectoryA`, `FindFirstFileA`, `FindFirstFileExA`, `GetDiskFreeSpaceA`, `GetFileAttributesExA`, `RemoveDirectoryA`, `GetTempFileNameW`, `GetFullPathNameW`, `GetDriveTypeA/W`, `GetLongPathNameA/W`, `GetShortPathNameW`, `GetVolumeInformationW`, `CopyFile2`, `CopyFileExW`, `CreateDirectoryExW`, `CreateHardLinkW`, `CreateSymbolicLinkW`, `MoveFileWithProgressW`, `ReplaceFileW`; in the CRT `_access`, `_chdir`, `_chmod`, `_mkdir`, `_rmdir`, `_stat64`, `_waccess`, `_wchdir`, `_wmkdir`, `_wrmdir`, `_wrename`, `rename`, the `_findfirst*`/`_findnext*` pairs, `_fullpath`, `_popen`, `_sopen`, `freopen` and every crt-time path call but `_wutime64`.
  Closes when: the remaining path-taking exports are translated too.
- `api-ms-win-core-processenvironment-l1-1-0.dll`, `api-ms-win-core-file-l1-2-0.dll` and the other contracts a GDK title may import path functions from are not replaced. Only `kernel32.dll`'s copies of `SetCurrentDirectoryA/W` and the file API set's copies of the file functions are translated; an import that binds to a different contract reaches Windows untranslated.
  Closes when: a title is seen importing a path function from a contract that has no forwarder.
- The CRT forwarders call the System32 `ucrtbase.dll`; a title that opens files through another CRT, such as `msvcp140.dll`'s `_Fiopen`, still sees its own paths.
  Closes when: the C++ stream open is intercepted as well.
- A drive stays mounted for the rest of the process even when the folder behind it is removed, and a title can mount no drive of its own.
  Closes when: mounts follow the folder's lifetime.

# kernelx

## change paths are translated

- `G:\` paths are translated through the shared drive table in `g6shim_runtime.dll`, in kernelx's own file functions and in `ucrtbase.dll`'s imports of nine file functions. Modules that call the PC's file functions directly need one of the file forwarder DLLs.

# shader

## add Functional

- A code-table read past the table's end returns its last dword; the console reads whatever memory follows the code.
  Closes when: a title relies on a constant table that runs into following memory.
- Translated loops leave after 2^20 iterations per entry; a genuine loop with more iterations is cut short.
  Closes when: the console's loop bound is proven finite for such a loop.
- `v_sin_f32`, `v_cos_f32` and their 16-bit forms use 2π for turns below 2^-12, a last-place difference from the console's scale.
  Closes when: a title compares those results bit for bit.
- LDS addresses are taken as dword-aligned and in bounds; out-of-bounds console accesses read zero and drop writes.
  Closes when: a title depends on the clamp.
- A quad mask is taken as "some lane of the quad": derivatives are right, and a side effect under that mask would not be.
  Closes when: a side effect lives under a quad mask.
- A global load of a byte or half is taken not to cross a dword.
  Closes when: a title loads a field across a dword boundary.
- A descriptor read as data reports a base level of 0 and stops the translation for its address, format and tiling dwords.
  Closes when: a title needs a descriptor's other dwords.
- Typed-format loads take the descriptor's channel swizzle as the identity, elements of 4 bytes or more as dword-aligned, and norm conversion as the host's division, which is 1 ULP off the hardware's rounding for some values.
  Closes when: a title compares converted values exactly.
- One immediate sampler used with and without comparison becomes a comparison sampler.
  Closes when: a title uses the same sampler words both ways.
- Mesh programs request the declared vertex and primitive maxima and cull primitives past the program's count by emitting all-zero indices, so the host sees unused vertex and primitive slots.
  Closes when: frame time shows the unused slots matter.
- A mesh group id arrives multiplied by `PSV0`'s thread count when the runtime info is version 2 or later, and by the group's lane count otherwise.
  Closes when: an older container's group id is observed.
- 16-bit vertex inputs are not unpacked; a read of one stops the translation.
  Closes when: a title reads a 16-bit vertex input.
- `v_fma_f32` and the f16 multiply-adds lower as `FMad` in f32, which may round twice.
  Closes when: a title depends on a single rounding.
- `v_rcp_f32` and its f16 form divide instead of using the hardware's approximate reciprocal.
  Closes when: a title compares reciprocals bit for bit.
- `v_cvt_u16_f16` and `v_cvt_i16_f16` convert through 32 bits, so out-of-range values wrap instead of saturating.
  Closes when: a title converts a value outside the 16-bit range.
- Inline f16 constants of a packed operation are taken to fill both halves.
  Closes when: a packed operation uses one half of such a constant.
- Image stores are taken to write all four channels of a float format; other mask patterns stop the translation.
  Closes when: a title stores fewer channels.
- 64-bit image and buffer atomics stop the translation.
  Closes when: a title uses one.
- `d16` image and buffer results and stores are taken as floats; integer 16-bit formats would keep 16 bits.
  Closes when: a title uses an integer 16-bit format.
- Typed buffer stores are taken to write all four channels of a float format.
  Closes when: a title stores fewer channels.
- A cube sample's view is taken as a cube, not a cube array; the face operand's array slice is ignored.
  Closes when: a title samples a cube array.
- Without the misc vector, `pos1` and `pos2` carry clip then cull distances in semantic order.
  Closes when: point size, render-target or viewport index exports are seen.
- The merged-wave info sets only the wave index (bits 24-27); the console's other bits are left zero.
  Closes when: a title reads those bits.
- The float `MODE` register is not modelled; `s_setreg_b32`, `s_setreg_imm32_b32` and `s_version` stop the translation.
  Closes when: a title sets the float mode.
- `s_swappc_b64` (the vertex fetch call) is not followed; its results arrive as inputs.
  Closes when: a title's fetch results are not inputs.
- A cross-lane operation in a mesh shader stops the translation: SM 6.6 has no mesh wave size.
  Closes when: a mesh program needs one.
- Packed shader records are found by their marker instead of through the file's header tables; a record whose two size fields disagree, whose unpacked size exceeds 64 MiB or whose container does not parse is skipped.
  Closes when: the header tables are decoded.
- `g6shim-shader` reports translated as 0 because nothing writes translated counts yet.
  Closes when: the translation pipeline writes its counts.

# shader

## add Functional

- `g6shim-shader` guesses a packed record's name from the graphic bytes before its NUL and leaves the record unnamed when no length byte matches.
  Closes when: the archive's name table is decoded.

# d3d12_xs

## add Silent stubs

- `ScheduleFrameEventX` returns `S_OK` without scheduling anything: every event kind happens once per tick.
- `GetFrameStatisticsX` returns no statistics.
- `CreateQueryHeap` keeps its description; no query produces a result while no GPU work runs.
- Render target and depth views are records in descriptor memory only; no host view exists yet.
- `CreateSamplerX` stores the console's 27-word description; it is not turned into a host sampler yet.
- `D3D12ConfigureVirtualMemoryX` logs its inputs and ignores them beyond reporting the user address range.

## add Unknown function info

- Device slots 0x230 and 0x238 take a resource description and a view description for memory the title placed; their argument order is inferred from call sites.
- Device slots 0x250 and 0x258 are page pools: `(address in 64 KiB pages, page count, handle out)` and `(handle)`.
- Device slot 0x3f8 is the enhanced-barrier committed-resource creation; its castable-format arguments are not decoded.
- Device slot 0x428 creates a resource over a title address: `(address, description, initial layout, clear value, iid, out)`.
- Device slot 0x360 is `GetResourceAllocationInfo2`: the plain description plus a sampler-feedback mip region, and a per-resource placement out.
- The console sampler description's word indices come from observation: filters at 1-3, mode at 5, address at 6-8, LODs at 9-10, bias at 11, anisotropy at 13, compare at 16, border at 17-20.
- The interface version `0x20004` and the `ID3D12Device11` creation interface are the only layout signs observed.

## add Functional

- Command queues, command allocators, command lists and pipeline states (device slots 8-12, 48, 62, 73 and 86) return `E_NOTIMPL` and log once until the host GPU parts fill them.
- Resources have no host GPU copy: buffers are not shadowed, textures are not uploaded, and no host transitions happen.
- Descriptor heap writes are not mirrored into a host heap; the shader-visible heap hooks do nothing.
- Committed resources take the title's memory with guessed allocation attributes (2 MiB pages from 2 MiB, alignment as log2, write-combined unless the heap is the default one).
- The page mapper is not registered; copies that map pages (command queues) are not implemented yet.
- Title memory remaps reach an observer that does nothing yet.
- Texture sizes come from the GFX10 layout engine, which omits compression metadata planes (HTILE, CMASK, FMASK, DCC).
- The adapter reports the title's memory split rather than measured memory, and its name is g6shim's.
- The display mode list is one fixed 3840x2160 60 Hz mode.
- `CheckFeatureSupport` says every use of every format is supported, and reports one quality level for 1, 2, 4 and 8 samples.
- Views of memory without a resource object are kept for the run; a resource's views go stale when it is released.
- A descriptor placement after the heap description is recognised only by its size matching the descriptor count.
- A blocking fence wait (null event) returns `E_NOTIMPL` instead of waiting.
- Root signature console flags (bits 24-31) are dropped; serialization and deserialization call the host's `d3d12.dll`.
- The device treats creation as `ID3D12Device11` as the sign of the newer command list and queue layouts.
- Only the first out-of-line flag word is set; which inline device calls it enables is not known.
- Root signature blobs are kept but not translated; pipeline state creation uses their host form later.
- The title's allocation hooks are used only when `xmem.dll` is loaded with `XMemAlloc`/`XMemFree`; otherwise resource memory comes from g6shim's own allocator.
- Host GPU failure reporting is not installed: there is no host device in this part.
- The resource area's host-upload test is not ported: it needs command lists, queues and the host GPU from later parts.

# d3d12_xs

## add Functional

- The host GPU waits for every submission, so CPU and GPU never overlap.
  Closes when: submissions are tracked per list with fences.
- Host diagnostics (removed-device data, GPU timestamps, per-list notes, descriptor slot descriptions, slow-list and copy reports) are not ported: a removed device logs its reason and the debug layer's first message, then ends the process.
  Closes when: a diagnostics build with the same switches is added.
- Presentation logs frames per second and milliseconds per frame only; draw counts, dropped commands and indirect counts wait for the command list and draw parts.
  Closes when: the draw and queue parts land.
- Frames shown on the host are not saved through the runtime's capture hook.
  Closes when: presentation is wired to the capture API.
- Buffer copies cover only the ranges draws are known to read; a buffer read through a directly indexed heap keeps what its first use copied.
  Closes when: descriptor tables name the ranges they read.
- Shader writes to a buffer's host copy are found by keeping the range's bytes before the submission and comparing them after it.
  Closes when: a host-side dirty mask makes the snapshot unnecessary.
- Physical-page views are guarded one 64 KiB block at a time; writes through another view of the same pages are not seen.
  Closes when: page-pool writes notify the watcher.
- A texture change detiles the whole texture; only plane 0 is uploaded; only textures read through descriptor tables are brought up to date; XOR layouts are not uploaded.
  Closes when: per-page detiling and directly indexed textures are handled.
- View heaps larger than the host's portable limit are capped, so title slots past the cap have no host descriptor.
  Closes when: a title is seen reaching the cap.
- A view of a typed texture in another format of its family reads without the conversion (sRGB of UNORM); placed unordered texture views are not mirrored.
  Closes when: such views are cast through castable formats.
- Texture change detection hashes memory with FNV-1a; only equality of hashes is used.
  Closes when: a shared content hash is available.
- Command queues, allocators, command lists, graphics, compute and stream pipelines are still `E_NOTIMPL` extension points.
  Closes when: the command list, queue, draw and pipeline parts land.
- `buffer_copy_to_host` refreshes a whole buffer into its host copy immediately; no title path calls it yet.
  Closes when: title copies are recorded as deferred steps.
