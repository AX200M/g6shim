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
