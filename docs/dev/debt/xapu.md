# xapu

## Build

None.

## Silent stubs

- `XApuEnqueueCommand` decodes no audio: a process command reports the packet's frame count at 48 kHz and writes silence, so every stream is silent.
  Closes when: an Opus decoder is available to the replacement DLL.

## Unknown function info

- `XApuConnect`'s `frames_per_output` must be zero; its meaning is not known.
- An activate command's `init`, `init_length`, `start_pitch`, `processing` and `processing_length`, and a process command's `pitch_ramp` and `target_pitch`, are not read.
- A result's `state`, `state_data`, `usec` and `pitch` are written as 0, 0, 0 and 1.0 without a confirmed console meaning.

## Functional

- Only the plain Opus decoding processing kind exists; the rate-conversion kinds, which the console also offers, get `E_NOTIMPL`.
- A packet is checked against its Opus framing and table-of-contents byte only; the payload is not decoded.
- The deinterleaved-output option is accepted and both sample layouts receive silence.
