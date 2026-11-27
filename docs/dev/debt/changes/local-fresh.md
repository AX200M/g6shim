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
- `g6shim-shader --prepare` fills only the translated-shader cache; host pipelines are still built during the run.
  Closes when: the prepare step also builds the title's pipelines from a recorded list.
