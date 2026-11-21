# kernelx

## Build

None.

## Silent stubs

- `XMemAlloc` ignores its attribute bits; blocks are zeroed and aligned to 4 KiB from 4 KiB on, else to 64 bytes.
  Closes when: the attribute bits are decoded.
  Title IDs: 3DD7389B
- `VirtualAlloc` drops type and protection bits the PC rejects, and retries a failed large-page request with normal pages.
  Title IDs: 1738CA2A, 3DD7389B

## Unknown function info

- `CreateXAudio2Object` is taken as `XAudio2Create` with a fourth argument; the flags and the fourth argument are logged and not passed on.
  Title IDs: 34BA1DE0
- The console-only `VirtualAlloc` type and protection bits are not decoded.

## Functional

- `G:\` paths are translated in kernelx's own file functions and in `ucrtbase.dll`'s imports of nine file functions. Other modules that call the PC's file functions directly get no translation.
- `WaitOnAddress`, `WakeByAddressAll` and `WakeByAddressSingle` are generated stubs; the PC exports them from `kernelbase.dll`.
  Closes when: they forward to `kernelbase.dll`.
