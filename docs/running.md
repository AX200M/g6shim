# Running a game

Work on a copy of the game folder. Deploying writes DLLs into it, and the game writes logs, saves and settings next to its executable.

## Workflow

```bat
:: system functions the game needs and how many g6shim provides
g6shim-scan D:\work\Game --hle hle

:: one replacement DLL project per missing DLL; names the PC's kernel32.dll also exports are forwarded
g6shim-stubgen D:\work\Game -o hle --forward kernel32.dll

:: route imports of DLLs Windows always loads itself (KnownDLLs) through g6shim_<dll>
g6shim-relink D:\original\Game.exe -o D:\work\Game\Game.exe --rename combase.dll
g6shim-stubgen D:\work\Game -o hle --forward combase.dll

:: build the DLLs and copy them next to the game
cmake --preset release -DG6SHIM_GAME_DIR=D:\work\Game
cmake --build --preset release --target g6shim_deploy

:: run it
g6shim-run D:\work\Game\Game.exe
```

`unimplemented: <dll>!<function>` in the output is the next function to write. [docs/dev/hle.md](dev/hle.md) explains how.

## GDK titles

GDK titles call the PC's file functions directly, so they need the virtual drive without `kernelx.dll`. Point their file imports at the forwarders:

```bat
g6shim-relink D:\original\Game.exe -o D:\work\Game\Game.exe ^
  --rename api-ms-win-core-file-l1-1-0.dll=g6shim_file.dll ^
  --rename api-ms-win-core-file-l2-1-0.dll=g6shim_file2.dll ^
  --rename api-ms-win-crt-filesystem-l1-1-0.dll=g6shim_crtfs.dll ^
  --rename api-ms-win-crt-stdio-l1-1-0.dll=g6shim_crtstdio.dll ^
  --rename api-ms-win-crt-time-l1-1-0.dll=g6shim_crttime.dll ^
  --rename kernel32.dll=g6shim_kernel32.dll
```

Each forwarder re-exports the whole DLL it replaces: names it does not translate reach Windows unchanged, and the path-taking ones rewrite `G:\` to the folder of the executable. `G:\common.rpf` is `<exe folder>\common.rpf`. Build and deploy the forwarders like the other replacement DLLs.

## Tools

| Tool | Does |
|---|---|
| `g6shim-scan <folder or files> [--hle <dir>] [--all] [--importers] [--json]` | Lists imports that neither Windows nor the game provides, with coverage `[x]` implemented, `[~]` stubbed, `[ ]` missing. Skips folders named `EmbeddedXvd`. |
| `g6shim-stubgen <folder or files> -o <hle dir> [--dll <name>] [--forward <dll>]` | Creates or extends `hle/<name>/`. Never overwrites; appends what is missing. |
| `g6shim-relink <input> -o <output> [--rename <dll>[=<new>]] [--keep-subsystem]` | Writes a patched copy with renamed imports and the Windows GUI subsystem. Refuses to write over its input. |
| `g6shim-run <exe> [--timeout <s>] [-v] [--log <file>] [-- <args>]` | Starts the game under a minimal debugger and prints debug output, DLLs loaded from outside System32, exceptions as module+offset, stowed WinRT errors and the exit code. Default timeout 30 s. |
| `g6shim-run -p <pid> -tid <tid> ...` | Attaches to a suspended process and resumes it, the way `plmdebug /enableDebug` starts a package debugger. |
| `g6shim-shader <folder or files> [--json <out.json>] [--title-id <8 hex digits>] [--out <dir>] [-v]` | Finds the shader containers a game ships, decodes their native programs, translates them and counts unique shaders per stage. `--json` writes the unique per-stage counts for the progress maps. `--out` writes each translated PC container as `<hash>.dxil` into that folder; a folder inside the repository is refused. |

## Environment variables

| Variable | Effect |
|---|---|
| `G6SHIM_LOG=<path>` | Log file. Default `g6shim.log` in the working directory (the game folder under `g6shim-run`). |
| `G6SHIM_LOG_STDERR=0` | Stop copying the log to stderr. The log always goes to the debugger output too. |
| `G6SHIM_TRACE=1` | Verbose diagnostics: repeated activations and queries, first calls of fast-table entries and draws, packet kinds, topology values. Graphics objects also remember their last 32 calls, and a command list that fails to close logs them. |
| `G6SHIM_CAPTURE_FRAMES=300,600` | Save the back buffer of those presented frames (counted from 1) as `g6shim_frame_<n>.bmp` next to the executable. Works with the window hidden or the desktop locked. |
| `G6SHIM_D3D_DEBUG=1` | Create the device with the Direct3D debug layer; invalid calls appear as debug output. |
| `G6SHIM_D3D_DRED=1` | Record GPU progress and page faults, so the log says where the GPU stopped when the device is removed. The reason for a removal is logged without it. |
| `G6SHIM_INPUT_AUTOPRESS=6:Menu,9:A` | Hold buttons for 0.2 s at those seconds after the first controller read. Buttons: `Menu View A B X Y Up Down Left Right`. |
| `G6SHIM_BCPACK_TABLES=<path>` | BCPack constant tables for DirectStorage texture decompression. Default: `bcpack-tables.bin` next to `XCRDAPI.dll`. Without a table file BCPack requests fail; zlib and plain requests still run. |
| `G6SHIM_DXIL=<path>` | `dxil.dll` the shader translator and its tests validate and sign DXIL with. Default: the newest `Windows Kits\10\bin\<version>\x64\dxil.dll`. Without one the validator tests are skipped; when the variable is set and the file cannot be loaded, they fail. CI sets it to the Windows SDK's copy. |
| `G6SHIM_CACHE_DIR=<folder>` | Folder for translated shaders and compiled host pipelines. Default `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>`. |

## Caches

Translated shaders and compiled host pipelines are kept outside the game folder so the game folder stays read-only:

| Folder | Content |
|---|---|
| `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>\shaders` | one entry per translated shader container |
| `%LOCALAPPDATA%\g6shim\cache\<title ID or exe name>\pipelines` | the host pipeline library and the list of title pipelines to start early |

`G6SHIM_CACHE_DIR` replaces the `<title ID or exe name>` folder. A changed build, adapter or driver discards the entries it cannot use; deleting the folders is always safe.

## Files next to the executable

| File | Content |
|---|---|
| `g6shim.log` | The log, appended on every run. |
| `g6shim_input.ini` | Keyboard and controller mapping, written with defaults on first start. XInput controllers need no mapping. |
| `g6shim_saves\<container>\<blob>` | Saves. Files larger than 64 MiB are treated as damaged. |
| `g6shim_persistent\` | Persistent local storage, made on the first request for it and mounted as its own drive on the highest letter of `D:`-`Z:` that neither the host nor another mount uses. The log names the letter. |
| `g6shim_frame_<n>.bmp` | Captured frames. |
| `console_xg_x.dll` | Optional: a copy of the console's own `xg_x.dll` from your dump. When present, texture layouts come from it instead of g6shim's own code. |

## Notes

- The console's game drive `G:\` is the game folder; for a GDK title it is the folder of the executable, and the file forwarders translate it.
- DLLs the game ships for its own WinRT classes are activated from the game folder. Some need the Visual C++ 2012 x64 runtime.
- A window close ends the game.
