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

- `G:` is mounted on the folder of the executable, persistent local storage on a free letter and the temporary drive on `T:`, but no title-facing call reports the persistent storage drive yet.
  Closes when: the persistent storage functions report their drive.
- The forwarders translate only the functions listed in `docs/abi/g6shim_file.md`. The other path-taking exports of the replaced DLLs stay plain forwarders: in the file sets `FindFirstFileA`, `GetDiskFreeSpaceA`, `GetFileAttributesExA`, `RemoveDirectoryA`, `GetTempFileNameW`, `GetDriveTypeA/W`, `GetLongPathNameA/W`, `GetShortPathNameW`, `GetVolumeInformationW`, `CopyFileExW`, `CreateDirectoryExW`, `CreateHardLinkW`, `CreateSymbolicLinkW`, `MoveFileWithProgressW`, `ReplaceFileW`; in the CRT `_access`, `_chdir`, `_chmod`, `_rmdir`, `_stat64`, `_waccess`, `_wchdir`, `_wrename`, `rename`, `_fullpath`, `_popen`, `_sopen`, `freopen` and every crt-time path call but `_wutime64`.
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
- 64-bit buffer atomics stop the translation.
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
- `buffer_copy_to_host` refreshes a whole buffer into its host copy immediately; no title path calls it yet.
  Closes when: title copies are recorded as deferred steps.

# xcrdapi

## add Functional

- BCPack lanes decode one symbol step at a time in the schedule's order, at roughly 30-60 MB/s for BC1 and 15-20 MB/s for BC7 on one core; dividing the input per lane and decoding the lanes apart is not done.
  Closes when: texture streaming needs more throughput.

# d3d12_xs

## add Functional

- Only vertex, mesh, pixel and compute stages get host pipelines; descriptions with domain, hull, geometry or amplification stages, and plain graphics descriptions, keep their shader containers only, and the 32-byte packet after a pipeline's header stays zero.
  Closes when: the other stages and the plain description are translated into host pipelines.
- Host pipelines are made on worker threads and waited for at most 500 ms per title frame; a draw or dispatch whose pipeline is not ready is dropped, so a pass run once whose pipeline takes longer is lost.
  Closes when: the wait budget is derived from a measurement rather than a guess.
- A depth-stencil subobject that carries the bounds test is compiled without it; the host pipeline gets the plain depth-stencil description.
  Closes when: a title enables the bounds test.
- Console rectangle and quad primitive types are compiled as host triangles.
  Closes when: a title needs the console's rectangle or quad rasterisation.
- `CreateComputePipelineStateX` accepts its console extension count and extensions but uses only the plain compute description.
  Closes when: a title's compute pipeline depends on the console's shader settings.
- Translated shaders and host pipelines are cached by content hash under `%LOCALAPPDATA%\g6shim\cache` (or `G6SHIM_CACHE_DIR`); the hashes are MD5, and a changed module file invalidates every entry.
  Closes when: a shared content hash is available.
- The pipeline library and the early-pipeline list are written when the workers run dry (at most every 20 s) or on an explicit save, so a run that ends while pipelines are still being made can lose that last batch.
  Closes when: every run saves at shutdown.

# shader

## add Functional

- `g6shim-shader --out` names every translated PC container after a 64-bit FNV-1a hash of the console container; when one console container yields PC containers for more than one stage, the later file overwrites the earlier one.
  Closes when: a wider or per-program digest names the files.
- `g6shim-shader --out` refuses a folder under a repository found from a `.git` entry above the working directory or the tool's folder; a repository found another way is not refused.
  Closes when: the source root is passed in or compiled in.
- `g6shim-shader` counts a container as translated when at least one of its native programs produced a PC container; the other programs' first errors are counted by reason under `-v`.
  Closes when: every native program's outcome gets its own container count.

# d3d12_xs

## add Silent stubs

- `SetMarkerX`, `BeginEventX` and `EndEventX` return without recording anything: only profilers read them.
- Queue `Wait` returns `S_OK`; nothing runs asynchronously on a queue, so a fence is never waited for there.
- List pipeline syncs, driver hints, depth order, shading rate and root signature setters do nothing; host lists order their own work and every submission is waited for.
- `BeginQuery`, `EndQuery` and the query heaps record nothing; resolved queries report a million occlusion samples, visible binary occlusion, the GPU clock for timestamps and zero pipeline statistics.
- The bottom-of-pipe 32- and 64-bit value waits do nothing; a value another queue or the CPU has to write first cannot be waited for.

## add Functional

- Bottom-of-pipe value writes and resolved query results are stored when the submission completes, on the CPU's timeline rather than the GPU's.
- The console flags of `DispatchX`, `CopyTextureRegionX`, `CopyResourceX` and `ResetX` are ignored.
- Draws, mesh dispatches and dispatches are dropped while the host pipeline of the bound title pipeline cannot be made: a shader that does not translate, a pipeline the host refuses, or a mesh pipeline on a host without mesh shaders.
  Closes when: every shader a title binds translates and the host accepts every pipeline.
- Indirect execution is dropped when the clamp shader does not compile or the buffer is not 4-byte aligned.
  Closes when: a title needs indirect execution without the clamp.
- The indirect clamp cuts legitimate work past 2^24 vertices, 2^26 vertex instances, 65,535 groups per dimension, 2^20 groups per dispatch and the list budget of 2^27 vertex instances or threads.
  Closes when: a title needs larger indirect work.
- Indirect mesh dispatches (a group count in GPU memory) and mesh dispatches with more than one group in y or z are dropped.
  Closes when: a title uses either.
- Console triangle fans, line loops, quads and quad strips are drawn as triangle lists; indexed rectangles are not drawn.
  Closes when: a title depends on the console's winding or on indexed rectangles.
- A rectangle's fourth vertex is computed from vertex inputs, not from vertex-shader outputs; formats other than float, half and unorm 8/16 keep the first vertex's value, as do components past the end of the vertex.
  Closes when: a rectangle's shader is not linear in its inputs.
- Texture-to-buffer copies are dropped (logged once); texture-to-texture and footprint-to-texture copies run.
  Closes when: a title reads a texture back through a copy.
- Split barriers run at their second half only, transition whole resources rather than subresources, and a barrier naming a buffer instead becomes one unordered-access barrier for the list.
  Closes when: subresource states or split barriers matter to a title.
- Enhanced barrier groups only order shader writes with one unordered-access barrier; their layouts are not applied.
  Closes when: a title depends on a specific enhanced-barrier layout.
- Page mappings are applied at once, outside the host queue; a failed mapping is logged once and the batch still reports success.
  Closes when: a title observes a page mapping before an earlier submission has run.
- `CopyMemoryToMemoryX` copies at submission when both ranges are committed title memory, outside the host GPU.
  Closes when: a title's copy must be ordered with its GPU work.
- Root views are taken as 64 KiB constant buffers and 1 MiB shader resources, cut at the buffer's end; constant buffers not 256-byte aligned in their host copy go through scratch memory.
  Closes when: a root view's real size is needed.
- A list keeps its upload memory and fresh descriptor tables until it is reset; a title that never resets a list accumulates them.
  Closes when: a title resets too seldom for the host's memory.
- A draw or dispatch with no dynamic descriptor slots for a constant-buffer table is dropped rather than reading the title's descriptors as host ones.
  Closes when: dynamic descriptor slots are not exhausted.
- Queues and lists log the frame, draws and dispatches only; dropped and indirect counts are not logged.
  Closes when: the diagnostics build lands.

# shader

## add Functional

- A per-lane descriptor joined at a loop header is refused (`a descriptor read from the first lane of a phi at a loop header`): what a lane holds after some passes of the loop is not traced.
- A waterfall loop's access is proven, but its other per-lane work is taken to run once for each lane; a loop body whose effect adds up over the passes a lane waits through would differ.

# project

## add Functional

- The illegal-instruction emulation covers `clzero`, `monitorx`, `mwaitx`, `clwb` and SSE4a's `extrq`/`insertq` only; other AMD-only instructions, such as `rdpru`, still raise the exception.
  Closes when: a title stops on another AMD-only instruction.
- `monitorx` does nothing and `mwaitx` yields the thread instead of waiting for the monitored line; a spin loop around them still ends.
  Closes when: a title needs the monitor's wait semantics.
- `clwb` is skipped and its write-back loop is retired in one exception; host memory is coherent, so no cache line is written back.
  Closes when: a title's timing depends on the write-back.

# xgameruntime

## add Functional

- `XLaunchNewGame` logs its executable and arguments and returns; the title is not ended and no new process starts.
  Closes when: the runner can restart a title with its own options and the title's arguments.
- `XTaskQueueTerminate` ignores its wait flag: it runs the queued callbacks as cancelled on the calling thread and returns without waiting for a callback that is already running.
- Feature checks answer from the console's full area list, so areas whose `QueryApiImpl` still returns `0x80070032` are reported as available.
  Closes when: every advertised area is served.
- The analytics information always describes the device as `Xbox Series X`; there is no console model selection.
- The device type the console keeps in `Software\Microsoft\Windows NT\CurrentVersion\OEM\ConsoleMode` is not served, so title code that reads it sees the host's registry and takes the device for a PC.
  Closes when: a registry replacement answers that value from the selected console model.

# d3d12_xs

## add Functional

- The window fit and its hooks cover the window the host presents on: other windows a title creates are neither fitted nor made to end the title when closed.
  Closes when: the console's window contract is replaced and every title window gets the behaviour.
- The app-state change registry has no exporter: no replaced contract forwards registrations to it, so a title's suspend routines are not run when its window closes.
  Closes when: the app-notify contract is replaced and forwards its registrations.
- Only suspension has a path: closing the window runs the registered routines; no resume notification reaches the title.
  Closes when: the console's resume notification is served.
- Focus is not tracked: the title keeps presenting while its window is in the background, and no notification tells it that it lost or gained focus.
  Closes when: the console's focus notifications are served.
- `G6SHIM_FULLSCREEN` fills the monitor's rectangle, so a title whose aspect ratio differs from the monitor's is stretched rather than letterboxed.
  Closes when: a title's frame has to keep its aspect ratio in fullscreen.
- `G6SHIM_WINDOW` forces the window's client size without fitting it; a size larger than the monitor puts part of the window off screen.
  Closes when: a window has to be larger than the monitor.

# project

## add Functional

- The Series S title-memory budget is the published 7,776 MiB standard configuration with no GPU-optimal range; later system updates gave those titles more, so a title that sizes from this figure sees a smaller budget than a current console.
  Closes when: the console's later Series S title-memory size is known.
