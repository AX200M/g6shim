# d3d12_xs

## add Functional

- A title descriptor write is applied to the host view at the next draw that uses the slot, or at the next submission for a heap a shader indexes, not while the GPU is still running an earlier submission.
  Closes when: a write during GPU execution of the current submission is visible to draws the GPU has not reached.
  Title IDs: 7952987A
