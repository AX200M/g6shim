# d3d12_xs

## add Functional

- A host slot is shown only after the upload that fills the texture it names, and new host memory waits until it is resident.
  Closes when: a slot sampled in the same submission as its first upload reads the uploaded texels, and a resource created not-resident is not bound before that wait.
  Title IDs: 7952987A
