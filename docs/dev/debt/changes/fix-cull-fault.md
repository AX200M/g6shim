# d3d12_xs

## add Functional

- A descriptor fetch still uses a zero heap base when the title never created a shader-visible view heap.
  Closes when: that dispatch is dropped instead of running.
  Title IDs: 7952987A
