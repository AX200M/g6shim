# shader

## add Functional

- A gather at an explicit level reads one mip: floor(max(level, 0) + 1/2), clamped to the view. The public instruction set says the user level replaces the computed level. It does not say whether that float is rounded or floored, or whether the sampler's level bias and clamps apply. Those are not applied.
  Closes when: the mip a gather at a fractional level reads is measured.
- A gather at an explicit level uses the sampler's address mode only when the sampler words are immediate. Any other sampler clamps. Border then returns the immediate border colour. A border colour that is not transparent black, opaque black or opaque white, and a border on a sampler that is not immediate, is not applied.
  Closes when: a gather at a level honours a sampler whose address mode is not immediate.
  Title IDs: 7952987A
