# d3d12_xs

## add Unknown function info

- BOX16 inner nodes are chosen for bottom-level builds with `MINIMIZE_MEMORY`, and sibling triangles are paired when neither `ALLOW_UPDATE` nor `PERFORM_UPDATE` is set; the console's own choice is not known.
- The stream description's records are not read.

## add Functional

- Invalid build inputs return `E_INVALIDARG` from the builder; the console throws `E_INVALIDARG` from functions that return nothing.
- Procedural (AABB) geometry, vertex formats other than `R32G32B32_FLOAT` and `R32G32_FLOAT`, index formats other than none, `R16_UINT` and `R32_UINT`, top-level descriptions laid out as an array of pointers, copy modes other than clone and compact, and postbuild types other than the two sizes get `E_NOTIMPL`; the console writes serialization info.
- Trees are built from Morton codes and collapsed to four children per node; the console's builder may group and order nodes differently, and its region order is not confirmed.
- The header's crc32 is written as 0.
- A bottom-level structure named by an instance must lie below console address 4 GiB inside the bound address space; otherwise the instance is treated as having none.
