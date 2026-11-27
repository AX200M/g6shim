# shader

## add Functional

- A descriptor word read as data that no root constant explains is read from the console descriptor words the host keeps beside each heap, which hold addresses, extents, strides, record counts, mip counts and image types only; its format, swizzle and tiling read as 0.
  Closes when: the words are encoded from the GFX10 resource and sampler descriptor layouts in full.
- A shader that reuses a descriptor register for data reads 0 for the descriptor word where both meet at a join, as the translator cannot express that word as data.
  Closes when: descriptor words are encoded in full, so the join carries the real word.
  Title IDs: 7952987A
- Buffer descriptors built at run time ignore their swizzle, add-thread-id and format words; float min and max atomics through runtime addresses compare NaN as an integer.
  Closes when: the full GFX10 buffer descriptor and float atomics are lowered.
  Title IDs: 7952987A
- An address in a hardware SGPR that holds no root parameter still stops the translation (`an address in an SGPR that is no root parameter`): its value is undefined, so it cannot go through the runtime-address table.

# d3d12_xs

## add Functional

- Console descriptor words carry addresses, extents, strides, record counts, mip counts and image types only: formats, swizzles, tiling and sampler words read as 0.
  Closes when: the words are encoded from the GFX10 resource and sampler descriptor layouts in full.
  Title IDs: 7952987A
- Console descriptor words exist for title slots only: a table the host copies into dynamic slots (one holding a constant view, or one past the title heap) reads its descriptor words as 0.
  Closes when: dynamic slots carry console descriptor words too.
- Console descriptor words are written when a slot is mirrored, into one buffer per heap, so a slot rewritten while an earlier submission still runs can show that submission the new words.
  Closes when: the words are kept per submission.
- A runtime title address that falls outside every registered buffer with a host copy (loose windows, textures, buffers not used yet) reads 0 and drops writes, and writes through runtime addresses into copied buffers stay in the host copy unless they fall at or past a root view of the same dispatch.
  Closes when: every title allocation a shader can address is copied both ways.
  Title IDs: 7952987A
- The first bind of a runtime-address table in a command list refreshes every title buffer with a host copy, and each draw or dispatch through the table snapshots each root view's host copy from the view's address to its end for copy-back.
  Closes when: the refresh and copy-back follow the ranges a shader can reach.
