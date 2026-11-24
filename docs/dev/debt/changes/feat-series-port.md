# chad

## add Unknown function info

- `XDspConnectWithMaximumStreamLimit` accepts a base block of 0x10000 to 0x1F400000 bytes, up to 0x80 seconds of impulse response, 0x100 streams and 2 channels; a null out parameter fails with `E_POINTER`, anything larger with `E_INVALIDARG`.
  Closes when: a title passes a request the console treats differently.
- `XDspActivate` accepts convolution streams of one or two channels and 0x200 or 0x400 frames without option 4, and other transforms of one channel and 0x200, 0x400 or 0x800 frames; option bits 2 and 8 require two channels. Null status and stream out parameters fail with `E_POINTER` before the other checks.
  Closes when: a title activates a stream shape g6shim refuses.

## add Functional

- `XDspSubmitCommand` completes its command when submitted and writes silence to the output block: the convolution and block transforms are not computed, because the coprocessor takes impulse responses in a partitioned frequency-domain layout that is not known, so reverb returns are dry.
  Closes when: that layout is observed.

# xapu

## add Silent stubs

- `XApuEnqueueCommand` decodes no audio: a process command reports the packet's frame count at 48 kHz and writes silence, so every stream is silent.
  Closes when: an Opus decoder is available to the replacement DLL.

## add Unknown function info

- `XApuConnect`'s `frames_per_output` must be zero; its meaning is not known.
- An activate command's `init`, `init_length`, `start_pitch`, `processing` and `processing_length`, and a process command's `pitch_ramp` and `target_pitch`, are not read.
- A result's `state`, `state_data`, `usec` and `pitch` are written as 0, 0, 0 and 1.0 without a confirmed console meaning.

## add Functional

- Only the plain Opus decoding processing kind exists; the rate-conversion kinds, which the console also offers, get `E_NOTIMPL`.
- A packet is checked against its Opus framing and table-of-contents byte only; the payload is not decoded.
- The deinterleaved-output option is accepted and both sample layouts receive silence.

# xg_xs

## add Functional

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

## add Unknown function info

- `XGSuggestSwizzleMode` ignores its flags argument.
- `XGCreateTextureComputer`'s copy-out slot ignores its sample index.
- A tail mip of the 4KB `R_X` and 64KB `R_X` modes reports swizzle mode 3, which the GPU does not otherwise use.

# xmem

## add Unknown function info

- `XMemAllocatePhysicalPages` numbers 64 KiB pages in 4 KiB frames: a page's number is a multiple of 16, and titles keep it as 21-bit `number >> 4`.
  Closes when: a title is seen keeping the numbers in another form.
- The `XMemAlloc` memory-type bits (29-32) and the other attribute bits are not decoded; only the alignment field (24-28) and the 32-bit address bit (33) are.
  Closes when: a title needs a memory type.

## add Functional

- `XMemVirtualAlloc` turns a 64 KiB-page reservation into a host placeholder; committing part of such a reservation later is not supported, because only a whole placeholder can be replaced.
  Closes when: a title commits part of a 64 KiB-page reservation.
- `XMemVirtualQuery` reports graphics memory (flags 3) and no tag for every committed page; the flags and tag a region was allocated with are not remembered.
  Closes when: a title tells CPU-only memory apart by these fields.
- `XMemAllocDefault` blocks come from the host heap above 2^40, unlike the console's; nothing sub-allocates them from a reservation below 2^40.
  Closes when: a title builds ray-tracing structures in this memory.
- `XMemGetWorkingSetStatistics` fills every field whatever `which` asks for, does not split use by bandwidth class, and reports GPU-optimal use as 0.
  Closes when: a title budgets from `gpu_optimal_used`.
- Title memory defaults to the high-memory configuration (10 GiB GPU-optimal, 3.5 GiB standard); nothing selects the small one (7,776 MiB standard).
  Closes when: the launcher selects a console configuration.
- `XMemTransferMemory` moves only title memory to the system (kind 0); tools transfers return 0, and `XMemIsToolsMemAvailable` reports none, because a retail console has no tools memory.
- A commit-limit failure logs the request and the system commit; it does not write a crash report.
  Closes when: the crash reporter takes the report.
