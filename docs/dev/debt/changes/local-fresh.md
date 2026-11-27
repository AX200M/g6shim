# upscaler_loader

## add Build

- The replacement needs `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` from the public upscaler SDK next to the game.
  Closes when: the deploy step copies them from a configured SDK folder.

## add Unknown function info

- The console backend device descriptor (`0x02000002`) is taken to share the host layout: header, then the device pointer.
  Closes when: a title run logs the console create chain and the host context is created from it.
  Title IDs: 7952987A

## add Functional

- Chained extensions on upscale dispatches, allocation callbacks and console-only create descriptors are dropped.
  Closes when: a title is seen passing one that changes the result.

# shader

## add Functional

- `g6shim-shader --prepare` fills only the translated-shader cache; host pipelines are still built during the run.
  Closes when: the prepare step also builds the title's pipelines from a recorded list.
- The ray-tracing node test always returns the triangle's barycentric numerators; the triangle-id return mode (descriptor dword 3, bit 24 clear) is not produced.
  Closes when: a title is seen clearing that bit, and the mode's result layout is observed.
- The ray-tracing node test returns a triangle miss for node kinds 6 and 7 (instance, procedural).
  Closes when: the console's result for those kinds is observed.

## add Unknown function info

- The ray-tracing node test's rounding, the order of two boxes entered at the same distance, and the result for a ray exactly through a shared edge (here: a hit in both triangles) are not confirmed against the console.
  Closes when: a capture of the console's results for such rays is compared.
  Title IDs: 7952987A

# d3d12_xs

## add Functional

- Copy-back writes what shaders changed in whole dwords; a title thread writing another byte of the same dword while the GPU ran loses that byte.
  Closes when: copy-back writes single bytes where the title changed the dword's other bytes.
- Bytes copy-back writes into title memory show as title writes at the next refresh and are uploaded again.
  Closes when: pages written only by copy-back are left out of the next refresh.
- With `G6SHIM_AMBIENT` set, the diffuse light of programs a local settings file names is raised to an estimate from the frame (or a constant), shaded in creases from depth, and the light the air scatters is capped; the console's traced light is not reproduced.
  Closes when: the title's own ray-traced light is right on the host and the stand-in is no longer needed.
- The stand-in's view of the camera (near plane 0.1, tan of half the vertical field 0.45 unless the settings file says otherwise) is assumed, not read from the title.
  Closes when: the camera is read from the title's constants.
