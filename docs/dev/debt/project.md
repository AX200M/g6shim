# project

## Build

- Package DLLs built with Visual Studio 2012 need the Visual C++ 2012 x64 runtime (`msvcr110.dll`, `msvcp110.dll`), which g6shim does not provide. Without it package activation fails and their classes become probes.
  Closes when: package activation names the missing dependency in its log line.
  Title IDs: 3DD7389B

## Silent stubs

None.

## Unknown function info

None.

## Functional

- No console Direct3D 12 module: `d3d12_x.dll` (`D3D12XboxCreateDevice` and its present path) does not exist yet.
  Closes when: a `d3d12_x` module creates a device and presents.
  Title IDs: 00F385F6
- The GDK runtime is not implemented: `MicrosoftGame.config` is not read and `XGameRuntime` has no replacement. Only titles with an `AppxManifest.xml` start.
  Closes when: a title with `MicrosoftGame.config` reaches its first frame.
- Crash about 6 s after start in a name-table lookup (exe+0xaace3b): a mount table entry holds a null archive pointer. The function at VA 0x140aacd80 is called from VA 0x140aae18d; the table is at `[[0x1414a4150]+0x78]`. The failed file opens before it are an existence check and an empty optional cache, not the cause.
  Closes when: the title gets past its loading screen.
  Title IDs: 3DD7389B
