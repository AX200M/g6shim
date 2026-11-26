# shader

## add Functional

- A descriptor heap index past the host heap selects a null descriptor of the same access kind, not the title's descriptor.
  Closes when: the host heap holds every title descriptor a shader can name.
  Title IDs: 7952987A
- A root view whose title address has no host copy is a zero-filled stand-in, so a read returns zeros instead of the title's bytes.
  Closes when: every root view address has a host copy.
  Title IDs: 7952987A
- Loops that run longer on the console are cut short; their results differ.
  Closes when: the console's loop bound is proven finite for such a loop.
  Title IDs: 7952987A
