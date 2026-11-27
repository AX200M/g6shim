# project

## add Functional

- Under `g6shim-run` every first-chance exception stops the game while the debugger looks at it. The shader translator throws about 90 C++ exceptions per shader on first use, about 2 ms per shader in a one-thread process.
  Closes when: first-use translation throws no exceptions on its common paths.
  Title IDs: 7952987A
