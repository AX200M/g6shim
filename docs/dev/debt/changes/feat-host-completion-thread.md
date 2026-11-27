# d3d12_xs

## add Functional

- A title queue wait is applied before the next submission on that queue is sent. There is one host queue, so a wait is not inserted as a host GPU wait between two host queues.
  Closes when: each title queue has a host queue and a wait is recorded on that queue.
- Shader-write copy-back covers declared ranges of title buffer copies. Mapping a buffer waits for those ranges. A host read of a texture that does not go through that map is not waited on.
  Closes when: every host read of a resource a pending submission may write waits for that submission.
- A removed device discards in-flight copy-back and does not publish the title fences that depended on it.
  Closes when: device removal completes those fences with the console's removed-device result.
