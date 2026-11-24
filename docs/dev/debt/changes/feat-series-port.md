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
