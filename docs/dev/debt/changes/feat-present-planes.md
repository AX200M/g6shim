# d3d12_xs

## add Unknown function info

- Present plane records are 72 bytes. Only the token, resource count and resource list are read; the two pointers after them and the parameters block (which holds the title's render and output sizes) are not.
  Closes when: the remaining plane fields and the parameters block are decoded.
  Title IDs: 7952987A

## add Functional

- Later present planes are scaled to the first plane and combined with premultiplied over (`out.rgb = over.rgb + under.rgb * (1 - over.a)`, `out.a = 1`). Source and destination rectangles, if a plane record has them, are not honoured.
  Closes when: the console compositor's blend, scaling and rectangles are confirmed.
  Title IDs: 7952987A
