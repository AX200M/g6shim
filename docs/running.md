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

## Tools

| Tool | Does |
|---|---|
| `g6shim-scan <folder or files> [--hle <dir>] [--all] [--importers] [--json]` | Lists imports that neither Windows nor the game provides, with coverage `[x]` implemented, `[~]` stubbed, `[ ]` missing. Skips folders named `EmbeddedXvd`. |
| `g6shim-stubgen <folder or files> -o <hle dir> [--dll <name>] [--forward <dll>]` | Creates or extends `hle/<name>/`. Never overwrites; appends what is missing. |
| `g6shim-relink <input> -o <output> [--rename <dll>[=<new>]] [--keep-subsystem]` | Writes a patched copy with renamed imports and the Windows GUI subsystem. Refuses to write over its input. |
| `g6shim-run <exe> [--timeout <s>] [-v] [--log <file>] [-- <args>]` | Starts the game under a minimal debugger and prints debug output, DLLs loaded from outside System32, exceptions as module+offset, stowed WinRT errors and the exit code. Default timeout 30 s. |
| `g6shim-run -p <pid> -tid <tid> ...` | Attaches to a suspended process and resumes it, the way `plmdebug /enableDebug` starts a package debugger. |

## Environment variables

| Variable | Effect |
|---|---|
| `G6SHIM_LOG=<path>` | Log file. Default `g6shim.log` in the working directory (the game folder under `g6shim-run`). |
| `G6SHIM_LOG_STDERR=0` | Stop copying the log to stderr. The log always goes to the debugger output too. |
| `G6SHIM_TRACE=1` | Verbose diagnostics: repeated activations and queries, first calls of fast-table entries and draws, packet kinds, topology values. |
| `G6SHIM_CAPTURE_FRAMES=300,600` | Save the back buffer of those presented frames as `g6shim_frame_<n>.bmp` next to the executable. Works with the window hidden or the desktop locked. |
| `G6SHIM_D3D_DEBUG=1` | Create the device with the Direct3D debug layer; invalid calls appear as debug output. |
| `G6SHIM_INPUT_AUTOPRESS=6:Menu,9:A` | Hold buttons for 0.2 s at those seconds after the first controller read. Buttons: `Menu View A B X Y Up Down Left Right`. |

## Files next to the executable

| File | Content |
|---|---|
| `g6shim.log` | The log, appended on every run. |
| `g6shim_input.ini` | Keyboard and controller mapping, written with defaults on first start. XInput controllers need no mapping. |
| `g6shim_saves\<container>\<blob>` | Saves. Files larger than 64 MiB are treated as damaged. |
| `g6shim_frame_<n>.bmp` | Captured frames. |

## Notes

- The console's game drive `G:\` is the game folder.
- DLLs the game ships for its own WinRT classes are activated from the game folder. Some need the Visual C++ 2012 x64 runtime.
- A window close ends the game.
