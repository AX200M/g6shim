# chad.dll

The console's audio DSP: streams that convolve blocks of float samples with an impulse response, or run other block transforms.

| Export | Signature and use |
|---|---|
| `XDspConnectWithMaximumStreamLimit` | `HRESULT (void* base, UINT32 base_length, UINT32 ir_seconds, UINT32 max_streams, UINT32 max_channels, void** out)`. The title connects once with a memory block for the DSP; accepted are base lengths 0x10000 to 0x1F400000, up to 0x80 seconds of impulse response, 0x100 streams and 2 channels. |
| `XDspDisconnect` | `HRESULT (void* dsp)`, after the streams |
| `XDspActivate` | `HRESULT (void* dsp, const StreamDesc* desc, const Status** status, void** stream)`. Fails with `E_OUTOFMEMORY` once `max_streams` streams are active. |
| `XDspDeactivate` | `HRESULT (void* stream)` |
| `XDspSubmitCommand` | `HRESULT (void* stream, const Command* command, UINT32* ticket)`. Completes the command at once; the optional ticket receives its number. |

## StreamDesc

Packed, 24 bytes: `kind`, `options`, `frames`, `channels` and `ir_floats` as UINT32, then the impulse response pointer at offset 20.

Kind 1 is convolution: one or two channels, 0x200 or 0x400 frames, option 4 refused. Other kinds are mono transforms of 0x200, 0x400 or 0x800 frames. Option bits 2 and 8 need two channels.

## Status

16 bytes: `submitted`, `completed` and `reserved` as UINT32, then the `HRESULT` result. The stream's owner polls `completed` for the ticket of the last completed command and `result` for its outcome, one block later than submission; a late command is read as an overrun.

## Command

32 bytes: two float gains, the input pointer, the output pointer and a reserved UINT64. The output block holds `frames * channels` floats.
