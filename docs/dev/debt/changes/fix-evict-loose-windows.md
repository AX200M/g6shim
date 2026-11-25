# d3d12_xs

## add Functional

- Copies of title memory without a resource object (4 MiB windows) are evicted after 512 completed submissions without a lookup; idleness, not what the title maps or frees, decides when they go.
  Closes when: the title's own unmapping and freeing of that memory end the windows.
  Title IDs: 7952987A
