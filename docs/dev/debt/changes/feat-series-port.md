# chad

## add Unknown function info

- `XDspConnectWithMaximumStreamLimit` accepts a base block of 0x10000 to 0x1F400000 bytes, up to 0x80 seconds of impulse response, 0x100 streams and 2 channels; a null out parameter fails with `E_POINTER`, anything larger with `E_INVALIDARG`.
  Closes when: a title passes a request the console treats differently.
- `XDspActivate` accepts convolution streams of one or two channels and 0x200 or 0x400 frames without option 4, and other transforms of one channel and 0x200, 0x400 or 0x800 frames; option bits 2 and 8 require two channels. Null status and stream out parameters fail with `E_POINTER` before the other checks.
  Closes when: a title activates a stream shape g6shim refuses.

## add Functional

- `XDspSubmitCommand` completes its command when submitted and writes silence to the output block: the convolution and block transforms are not computed, because the coprocessor takes impulse responses in a partitioned frequency-domain layout that is not known, so reverb returns are dry.
  Closes when: that layout is observed.
