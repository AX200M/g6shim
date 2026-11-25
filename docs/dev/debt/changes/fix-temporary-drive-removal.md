# project

## add Functional

- The temporary folder can remain after `g6shim_runtime.dll` unloads when a file inside it stays open without delete sharing for longer than the removal retries.
  Closes when: the folder is removed once every handle to it has closed.
