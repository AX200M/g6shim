# AGENTS.md

g6shim runs console games on Windows PCs without emulation. It provides replacement system DLLs with the console's file names and implements them one function at a time.

## Hard rules

1. **No game titles.** Refer to a game only by the 8-digit hex title ID from its manifest (`MicrosoftGame.config` or `AppxManifest.xml`). This covers code, log strings, test names, file names, docs, commits, issues and PRs.
2. **No trademarks in names or titles:** project and repository names, headings, file and folder names, commit subjects, PR and issue titles, labels. Platform, API and DLL names appear only where interoperability requires them: identifiers in code (DLL names, `Windows.Xbox.*` namespaces, export names), and the README's sentence on compatibility and its disclaimer.
3. **Clean-room.** Use only public documentation, the Windows SDK and observed behaviour. Never use, search for or paste leaked SDK, XDK or GDK material.
4. **No game material.** No game files, dumps, keys or decryption code, and no instructions for obtaining, dumping or decrypting games.
5. **Original game folders are read-only.** Deploy and run only in copies.
6. **Windows and MSVC only.** Builds stay warning-free at `/W4`.

## Build and test

```bat
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`debug` presets exist for all three. Before committing:

```bat
python tools/check_conventions.py --staged
python tools/debt_fold.py --check
```

## Layout

```
src/core/          PE reader, game-folder inventory, .def handling, relink
src/tools/<tool>/  g6shim-scan, g6shim-stubgen, g6shim-relink, g6shim-run
runtime/           g6shim_runtime.dll: logging, stubs, the shared main window
winrt/             static library: WinRT object kit (kit/) and console stand-ins by namespace
hle/<dll>/         one replacement DLL each; the .def is its coverage record
tests/unit/        unit tests, one file per area, shared CHECK header
tests/integration/ the fake-game pipeline
docs/              running.md, abi/<dll>.md, dev/
tools/             convention, ledger and coverage scripts; git hooks
```

## Conventions

- Read untrusted bytes only through bounds-checked readers, cap every table size and report damage as `pe::ParseError`. A malformed file never crashes.
- Add or update unit tests with every change; keep each test under 30 s.
- The `.def` files are the single source of truth for coverage. Generated stubs log their first call.
- A function that reports success without doing the work is a silent stub: list it in the ledger. When unsure, return an error and log it.
- Every PR adds a ledger fragment `docs/dev/debt/changes/<branch>.md` ([format](docs/dev/debt/README.md)); never edit the ledger files directly.
- Behaviour is general, never per title. No title ID checks in code.
- No comments except the two SPDX lines at the top of each source file (`SPDX-FileCopyrightText`, `SPDX-License-Identifier`), `#endif` markers and closing-namespace markers. Put knowledge in named constants, structs and small functions; put observed layouts in `docs/abi/<dll>.md`.
- Commit subjects are Conventional Commits with a module scope, e.g. `feat(kernelx): ...`, signed off with `git commit -s`.
- Logs, screenshots and notes go in the PR, never in the repository.
