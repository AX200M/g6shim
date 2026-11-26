# d3d12_xs

## add Functional

- A title buffer at or above the video-memory size threshold is kept in video memory. Shader writes are returned by 64 KiB piece, and only for pieces a submission declared written. A store the title did not declare is not copied back until a declared write shares its piece. Those copies are capped at 2048 MiB.
  Closes when: every shader write is declared, or an undeclared write is found without hashing the piece.
