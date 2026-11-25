# d3d12_xs

## add Functional

- A title window owned by another thread is resized when its owner next pumps messages, not when presentation starts; a title that never pumps keeps its own size.
  Closes when: a title needs its window sized before it pumps messages.
  Title IDs: 7952987A
