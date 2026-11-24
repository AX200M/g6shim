# xapu.dll

The console's audio decoder. A title connects once with a block of its memory, queues per-stream commands (activate, process, reset, deactivate) and dequeues one result per command. The command is carried out when it is queued, so its result is ready at once and results come back in command order.

## Exports

| Export | Signature and use |
|---|---|
| `XApuConnect` | `HRESULT (const ConnectInput*, ConnectOutput*, Connection**)`. Starts one processing kind and hands back the title's decoder memory and the optional result event. |
| `XApuDisconnect` | `HRESULT (Connection*)` |
| `XApuEnqueueCommand` | `HRESULT (Connection*, const CommandId*, UINT32* total)`. `total` receives the number of commands queued so far and may be null. |
| `XApuDequeueResult` | `HRESULT (Connection*, Result*)` |
| `XApuGetResultCount` | `UINT32 (Connection*, UINT32* total)`. Returns the results not dequeued; `total` receives the number dequeued so far. |

## Connect input (0x18 bytes)

| Offset | Field |
|---|---|
| +0x00 | `UINT32 process`; 1 is Opus decoding, other kinds get `E_NOTIMPL` |
| +0x04 | `UINT32 streams`; 1 to 500 |
| +0x08 | `UINT32 commands_per_stream`; 1 to 16 |
| +0x0C | `UINT32 memory`; bytes of untrusted memory to hand back, non-zero |
| +0x10 | `UINT32 frames_per_output`; must be zero |
| +0x14 | `UINT32 options`; bit 2 (value 4) asks for deinterleaved output, bit 3 (value 8) asks for a result event |

## Connect output (0x10 bytes)

| Offset | Field |
|---|---|
| +0x00 | `void* memory`; zeroed memory of the requested size, 16-byte aligned |
| +0x08 | `void* result_event`; an auto-reset event when option value 8 was set, otherwise null |

## Command id (0x18 bytes)

| Offset | Field |
|---|---|
| +0x00 | `UINT32 kind`; 1 activate, 2 deactivate, 3 process, 4 notify, 5 reset |
| +0x04 | `UINT32 stream`; below the connection's stream count |
| +0x08 | `UINT32 sequence`; echoed in the result |
| +0x0C | `UINT32 options`; value 1 signals the connection's event when the result is queued |
| +0x10 | `UINT64 context`; echoed in the result |

## Activate command (0x38 bytes)

| Offset | Field |
|---|---|
| +0x00 | `CommandId id` |
| +0x18 | `UINT32 channels`; 1 or 2 |
| +0x1C | `UINT32 init_length`; not read |
| +0x20 | `const void* init`; not read |
| +0x28 | `float start_pitch`; not read |
| +0x2C | `UINT32 processing_length`; not read |
| +0x30 | `void* processing`; not read |

## Process command (0x40 bytes)

| Offset | Field |
|---|---|
| +0x00 | `CommandId id` |
| +0x18 | `float pitch_ramp`; not read |
| +0x1C | `float target_pitch`; not read |
| +0x20 | `UINT32 first_frame`; first frame kept from the packet, clamped to its length |
| +0x24 | `UINT32 frame_count`; most frames written |
| +0x28 | `UINT32 input_length`; bytes of the Opus packet |
| +0x2C | `const UINT8* input` |
| +0x34 | `UINT32 output_length`; bytes available at `output` |
| +0x38 | `UINT8* output`; float samples, interleaved or deinterleaved per the connect options |

The packet's duration at 48 kHz comes from its table-of-contents byte and frame count, and is at most 5760 frames (120 ms). The frames kept are the smaller of the command's `frame_count`, the frames left after `first_frame`, and `output_length / (4 * channels)`.

## Result (0x50 bytes)

| Offset | Field |
|---|---|
| +0x00 | `CommandId id`; the command's id |
| +0x18 | `UINT32 state` |
| +0x1C | `UINT64 state_data` |
| +0x24 | `UINT64 usec` |
| +0x2C | `HRESULT error` |
| +0x30 | `UINT32 frames`; frames written |
| +0x34 | `UINT32 output_length`; bytes written |
| +0x38 | `UINT8* output`; the command's output |
| +0x40 | `float pitch`; 1.0 |
| +0x44 | `UINT32 input_length`; the command's input length |
| +0x48 | `const UINT8* input`; the command's input |

## Error codes

| Value | Meaning |
|---|---|
| `0x80070057` | `E_INVALIDARG` |
| `0x80004001` | `E_NOTIMPL`; processing kind not implemented |
| `0x8007000E` | `E_OUTOFMEMORY`; decoder memory could not be allocated |
| `0x8AC81000` | the stream already holds `commands_per_stream` results |
| `0x8AC81001` | no result queued yet; titles poll for it |
| `0x8AC81002` | unknown command kind |
| `0x8AC81003` | the stream is not activated |
| `0x8AC81005` | the stream is already activated |
| `0x8AC81008` | malformed Opus packet |

Errors for a queued command are reported in its result; `XApuEnqueueCommand` also returns success for them. An unknown command kind is the exception: it is refused and queues no result.
