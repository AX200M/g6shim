# shader

## add Functional

- A descriptor word read as data that no root constant explains is read from the console descriptor words the host keeps beside each heap, which hold addresses, extents, strides, record counts, mip counts and image types only; its format, swizzle and tiling read as 0.
  Closes when: the words are encoded from the GFX10 resource and sampler descriptor layouts in full.
- A shader that reuses a descriptor register for data reads 0 for the descriptor word where both meet at a join, as the translator cannot express that word as data.
  Closes when: descriptor words are encoded in full, so the join carries the real word.
  Title IDs: 7952987A

# d3d12_xs

## add Functional

- Console descriptor words carry addresses, extents, strides, record counts, mip counts and image types only: formats, swizzles, tiling and sampler words read as 0.
  Closes when: the words are encoded from the GFX10 resource and sampler descriptor layouts in full.
  Title IDs: 7952987A
- Console descriptor words exist for title slots only: a table the host copies into dynamic slots (one holding a constant view, or one past the title heap) reads its descriptor words as 0.
  Closes when: dynamic slots carry console descriptor words too.
- Console descriptor words are written when a slot is mirrored, into one buffer per heap, so a slot rewritten while an earlier submission still runs can show that submission the new words.
  Closes when: the words are kept per submission.
