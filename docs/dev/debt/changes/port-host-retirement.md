# d3d12_xs

## add Functional

- Title descriptor heaps and the resources a slot keeps are not moved onto the host retirement list when the title releases the heap. Slot replacement still uses the descriptor clock.
  Closes when: descriptor heap drop and slot replacement call host retirement, and a heap released during a submission stays until that fence plus the bury lag completes.
- A title heap is title memory. Releasing it does not retire a host GPU object, because a heap has none.
  Closes when: a host object created for a title heap, if one is added, is retired with the heap.
