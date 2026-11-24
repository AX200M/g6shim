# g6shim file forwarders

## Virtual drives

`G:` is mounted on the folder of the executable when the drive table is first used.

| Path form | Translated |
|---|---|
| `X:\...` | yes |
| `X:/...` | yes |
| `X:` | yes, the host root plus a separator |
| `X:name` | yes, `name` under the host root |
| `\\?\X:\...` | yes, output keeps the `\\?\` prefix |

A path whose letter is not mounted, a relative path and a null path are passed to Windows unchanged.

| Item | Value |
|---|---|
| Package drive | `G:` |
| Long-path cut-off | host root + tail from 248 UTF-16 units |
| Long-path form | `\\?\...`; `/` becomes `\`, empty and `.` parts drop, `..` pops a part and stops at the root |
| UNC long-path form | `\\server\share` becomes `\\?\UNC\server\share` |
| Separators | `\` and `/`; a non-verbatim result keeps the tail's separators |
| Free-letter mounts | highest of `D:`-`Z:` that neither the host nor another mount uses |
| Fixed-letter mount | replaces an earlier mount of that letter and hides a host drive of the same letter |

## Replacements

| DLL | Replaces | Path functions translated |
|---|---|---|
| `g6shim_file.dll` | `api-ms-win-core-file-l1-1-0.dll` | `CreateFileA/W`, `CreateDirectoryA/W`, `DeleteFileA/W`, `FindFirstFileW`, `FindFirstFileExA/W`, `GetDiskFreeSpaceW`, `GetDiskFreeSpaceExW`, `GetFileAttributesA/W`, `GetFileAttributesExW`, `GetFullPathNameW`, `RemoveDirectoryW`, `SetFileAttributesW` |
| `g6shim_file2.dll` | `api-ms-win-core-file-l2-1-0.dll` | `CopyFile2`, `MoveFileExW` |
| `g6shim_crtfs.dll` | `api-ms-win-crt-filesystem-l1-1-0.dll` | `remove`, `_wremove`, `_mkdir`, `_wmkdir`, `_wrmdir`, `_findfirst32`, `_findfirst32i64`, `_findfirst64`, `_findfirst64i32`, `_wfindfirst32`, `_wfindfirst32i64`, `_wfindfirst64`, `_wfindfirst64i32`, `_stat64i32`, `_wstat64`, `_wstat64i32`, `_wchmod` |
| `g6shim_crtstdio.dll` | `api-ms-win-crt-stdio-l1-1-0.dll` | `fopen`, `fopen_s`, `_wfopen`, `_wfopen_s`, `_wfsopen`, `_open`, `_wopen` |
| `g6shim_crttime.dll` | `api-ms-win-crt-time-l1-1-0.dll` | `_wutime64` |
| `g6shim_kernel32.dll` | `kernel32.dll` | `CopyFile2`, `CopyFileW`, `CreateFileA/W`, `CreateFile2`, `CreateDirectoryA/W`, `DeleteFileA/W`, `FindFirstFileW`, `FindFirstFileExA/W`, `GetDiskFreeSpaceW`, `GetDiskFreeSpaceExW`, `GetFileAttributesA/W`, `GetFileAttributesExW`, `GetFullPathNameW`, `MoveFileExW`, `RemoveDirectoryW`, `SetCurrentDirectoryA/W`, `SetFileAttributesW` |

Every other export forwards to the replaced DLL under the name it uses there.

| Item | Value |
|---|---|
| Narrow path | ANSI code page to UTF-16, translated once, then the wide twin is called |
| Narrow mode string | each byte becomes one UTF-16 unit, not decoded |
| `GetDiskFreeSpaceW` | the host path is cut to its volume root before the call |
| CRT host calls | resolved from the System32 `ucrtbase.dll` by name |
| Narrow find results | `FindFirstFileExA` and the narrow `_findfirst*` search through the wide twin; file names come back converted from UTF-16 to the ANSI code page, and the handle serves the narrow `FindNextFileA` / `_findnext*` |
| `GetFullPathNameW` | stays on the console drive: a mounted `X:name` is read as `X:\name`, then normalised as Windows does; the result is not translated to the host folder |
