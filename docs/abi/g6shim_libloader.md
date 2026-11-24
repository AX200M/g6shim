# g6shim library-loader forwarders

| DLL | Replaces |
|---|---|
| `g6shim_libloader.dll` | `api-ms-win-core-libraryloader-l1-2-0.dll` |
| `g6shim_libloader2.dll` | `api-ms-win-core-libraryloader-l1-2-1.dll` |

## Redirect

`LoadLibraryExW`, `LoadLibraryExA`, `LoadLibraryW` and `LoadLibraryA` load `g6shim_xgameruntime.dll` from the folder of the executable when the requested base name is `xgameruntime`, with or without `.dll` and in any case. The path is ignored, as are `LoadLibraryExW`'s file handle and flags. Every other name is passed to Windows unchanged.

| Exports in `g6shim_libloader.dll` | Exports in `g6shim_libloader2.dll` |
|---|---|
| `AddDllDirectory`, `DisableThreadLibraryCalls`, `EnumResourceLanguagesExA/W`, `EnumResourceNamesExA/W`, `EnumResourceTypesExA/W`, `FindResourceExW`, `FindStringOrdinal`, `FreeLibrary`, `FreeLibraryAndExitThread`, `FreeResource`, `GetModuleFileNameA/W`, `GetModuleHandleA`, `GetModuleHandleExA/W`, `GetModuleHandleW`, `GetProcAddress`, `LoadLibraryExA/W`, `LoadResource`, `LoadStringA/W`, `LockResource`, `RemoveDllDirectory`, `SetDefaultDllDirectories`, `SizeofResource` | `FindResourceW`, `LoadLibraryA/W` |

`LoadStringA` and `LoadStringW` forward to `user32.dll`; every other export forwards to `kernel32.dll`.
