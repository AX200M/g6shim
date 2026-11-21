# kernelx.dll

## Exports

| Export | Signature and use |
|---|---|
| `CreateFileW`, `CreateFile2`, `FindFirstFileExW` and the other file functions | PC signatures; paths on `G:\` name the game's package |
| `VirtualAlloc` | PC signature; accepts type and protection bits the PC rejects; large-page requests |
| `XMemAlloc` | `void* (SIZE_T size, ULONGLONG attributes)` |
| `XMemFree` | `void (void* p, ULONGLONG attributes)` |
| `LoadLibraryExW` | Loads console system DLLs with `LOAD_LIBRARY_SEARCH_SYSTEM32` |
| `GetProcAddress` | Looks up `CreateXAudio2Object` in the PC's `xaudio2_9.dll` |

## CreateXAudio2Object

| Item | Value |
|---|---|
| Signature | `HRESULT (IXAudio2** engine, UINT32 flags, UINT32 processor, void* extra)` |
| Engine vtable | XAudio2 2.9 layout: `CreateSourceVoice` slot 5, `CreateMasteringVoice` slot 7 |
