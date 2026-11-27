# d3d12_xs

## add Functional

- A draw or dispatch that runs with no sampler heap bound reads the title's largest sampler heap, and a dispatch with no view heap reads its largest view heap.
  Closes when: the heap a shader indexes is chosen from the heap address in its user data.
  Title IDs: 7952987A
