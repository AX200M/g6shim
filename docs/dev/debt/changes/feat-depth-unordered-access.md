# d3d12_xs

## add Functional

- A depth texture that is also an unordered-access target is copied back from its colour twin for mip 0 of array slice 0 only. Other mips and slices stay at the depth clear.
  Closes when: every mip and slice written through an unordered view is copied back.
- A 24-bit depth unordered twin is `R32_FLOAT` in [0, 1], not a packed 24-bit unorm, so a store of packed 24-bit integers does not match a later depth test.
  Closes when: 24-bit depth unordered writes keep the 24-bit unorm bits.
- A depth unordered twin is not refreshed from later depth-stencil draws. An unordered read sees the twin, not depth the rasterizer wrote after the last copy.
  Closes when: depth-stencil writes are copied into the twin before an unordered read.
- Multisampled depth textures and depth textures that are not 2D are not given unordered twins.
  Closes when: those textures accept an unordered view and the written samples are visible to a later depth test.
- Stencil unordered writes use one draw per stencil bit when the host does not support `PSSpecifiedStencilRefSupported`. That path is untested on such a host.
  Closes when: a host without specified stencil reference copies a stencil unordered write.
