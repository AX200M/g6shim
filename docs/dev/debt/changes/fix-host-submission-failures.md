# d3d12_xs

## add Functional

- A host submission whose fence signal, completion event or wait does not succeed leaves shader writes and deferred values unpublished. The resources of that submission stay allocated until a later fence completion on the same queue, or until the device is removed.
  Closes when: an unfinished submission is completed or retried and its shader writes are published once the GPU has produced them.
