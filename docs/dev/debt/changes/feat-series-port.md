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
