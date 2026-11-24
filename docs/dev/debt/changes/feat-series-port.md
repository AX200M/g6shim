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
