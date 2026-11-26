# shader

## add Functional

- A pixel wave read that is not a same-quad access still uses a DXIL wave operation, so a helper lane does not read 0.
  Closes when: helper lanes read 0 from wave operations that are not quad reads.
