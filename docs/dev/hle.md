# Replacement DLLs

One folder per replacement DLL in `hle/`. `g6shim-stubgen` creates it; the code is filled in by hand.

```
hle/<name>/
  <name>.def        exports: the single record of coverage
  <name>_stubs.cpp  generated stubs
  *.cpp             implementations, every file built automatically
  CMakeLists.txt    builds <name>.dll; links g6shim::winrt or Windows libraries when needed
```

## Export states

| `.def` line | State |
|---|---|
| `Foo=g6shim_stub_3` | Stubbed: logs its first call, returns 0 |
| `Foo=g6shim_Foo` | Implemented in this folder |
| `Foo=kernel32.Foo` | Forwarded to the PC's DLL |
| not listed | Missing |

`python tools/def_coverage.py` prints the counts per DLL.

## Implementing a function

1. Write it as `extern "C"` in any `.cpp` in the folder.
2. Point its `.def` line at it: `Foo=g6shim_stub_3` becomes `Foo=g6shim_Foo`.
3. Delete its `G6SHIM_STUB` line.
4. Add tests for any logic that does not need the game: put it in a header or function that `tests/unit/` can include.

## Stubs

Console code is x64, where the caller removes its own arguments, so a stub without parameters is safe to call. Its return value is 0 in `rax`:

- 0 is `S_OK` for `HRESULT` functions but `FALSE` or `nullptr` elsewhere, so a game may take a success path with nothing behind it.
- A function returning `float` or `double` gets whatever is in `xmm0`.
- Exported variables (`DATA`) cannot be stubbed.

A hand-written function that reports success without doing the work is a silent stub and goes in the ledger ([debt/README.md](debt/README.md)). When the behaviour is unknown, return an error and log it rather than make up data.

## WinRT classes

`winrt/` holds the object kit and the stand-ins for console classes, grouped by namespace. Activation reaches it through `g6shim_vccorlib140.dll`.

- A stand-in registers a vtable per class and interface: `register_interface(class, iid, make_vtable({{method, fn(&Function)}}))`. Method 0 is vtable slot 6.
- Anything not registered is a probe: it logs each call by slot and returns `E_NOTIMPL`. Calls shaped like an event's add or remove are served as such.
- `G6SHIM_TRACE=1` adds every repeated activation and query to the log.

Observed layouts go in [docs/abi](../abi/); gaps go in the ledger.
