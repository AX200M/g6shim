# shader

## add Functional

- A whole-quad lane mask read as data stops the translation.
  Closes when: the mask is lowered with a quad vote.
- An opaque lane mask read as data stops the translation.
  Closes when: the mask's bits are a per-lane condition.
