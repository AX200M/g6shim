# d3d12_xs

## add Functional

- A buffer view the title creates with no elements becomes a null host view: reads return zero and writes are dropped. The console's handling of such views has not been observed.
  Closes when: the console's behaviour for zero-length buffer views is observed and matches or is reproduced.
  Title IDs: 7952987A
