# advapi32.dll

## Console mode

`g6shim_advapi32.dll` answers the value code reads to tell a console from a PC. Every other export forwards to `advapi32.dll` under the name it uses there.

| Item | Value |
|---|---|
| Key | `HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\OEM` |
| Value | `ConsoleMode` |
| Type | `REG_DWORD` |
| Larger console (`series-x`) | `0x64` |
| Smaller console (`series-s`) | `0x60` |
| Missing | a PC; code that reads it behaves as it does on a PC |

`RegOpenKeyW` and `RegOpenKeyExW` return a pseudo handle for the key; `RegQueryValueExW` answers the value for that handle, and `RegCloseKey` accepts it. Any other key and any other value name goes to the host's `advapi32.dll`.
