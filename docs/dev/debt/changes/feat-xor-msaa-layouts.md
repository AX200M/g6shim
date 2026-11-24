# xg_xs

## remove Texture addressing is implemented for linear and for the single-sample modes without pipe or bank XOR

## add Unknown function info

- The address configuration default is 16 pipes, 8 packers, a 256-byte pipe interleave and 8 compressed fragments; it is not the console's measured value.

## add Functional

- 4 KiB `R_X` has no element equation, so its offsets and copies still fail.
  Closes when: an equation for that mode is known.
- A pipe interleave other than 256 bytes is stored, and XOR element offsets and copies stay unavailable.
  Closes when: equations for the other interleaves are known.
