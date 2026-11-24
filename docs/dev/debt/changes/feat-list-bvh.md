# d3d12_xs

## add Unknown function info

- Command-list slot 122 is taken to be the batched acceleration-structure build `(count, 0x50 descriptions)`. The other extra method of that list version, slot 123, is unknown.
- `GetRaytracingAccelerationStructurePrebuildInfo` has no slot in the known device table.

## add Functional

- Command-list acceleration builds, copies and postbuild queries use one 8 MiB host window. An address that is not an offset in that window, and not a pointer into its upload mapping, is refused. A title resource's GPU address is its CPU pointer, so a build that names one does not run.
- A bottom-level address stored in an instance description is used as a window offset. A title pointer written there is not rewritten, so the instance has no bottom-level structure unless that pointer is already an offset in the window.
- List methods that the console throws `E_INVALIDARG` from log the refusal and return. They do not throw.
- Two command lists recording acceleration commands at once can disagree about the window's state.
- The stream description in a batched build description is not read.
