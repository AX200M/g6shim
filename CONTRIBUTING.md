# Contributing

## Clean-room declaration

By contributing you declare that your work comes only from public documentation, the Windows SDK and behaviour you observed yourself, and that you have not used, read or copied leaked SDK, XDK or GDK material. Contributions must not contain game files, dumps, keys or decryption code, or explain how to obtain, dump or decrypt games.

## Licence

Contributions are licensed under GPL-3.0-or-later. New source files start with the same two SPDX lines as the others.

## Setup

```bat
git config core.hooksPath tools/hooks
```

The hooks run `tools/check_conventions.py` on staged files and on each commit message. A gitignored `.local/titles.txt` (one title per line) adds a title check.

## Pull requests

- Claim the work first in the coordination issues: [#2](https://github.com/gookie-dev/g6shim/issues/2) for work without game files, [#3](https://github.com/gookie-dev/g6shim/issues/3) for work with your own title files. Comment `claiming: <ID>`, or `finding: <one line>` for work you found but will not do.
- One topic per PR, based on current `main`. Follow-up work goes in a new PR.
- All tests pass: `ctest --preset release`. New behaviour comes with tests.
- At most three open PRs per author.
- Fill in every line of the PR template, including `AI-assisted: yes` or `no`. The author answers for every line either way.
- Add a ledger fragment `docs/dev/debt/changes/<branch>.md` ([format](docs/dev/debt/README.md)).
- Logs, screenshots and notes go in the PR description or comments, not in the repository.

## Commits

- [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): subject`, at most 72 characters. The scope names a module (`kernelx`, `d3d11_x`, `winrt`, `core`, `run`), never a game.
- Sign off every commit (`git commit -s`). The sign-off certifies the [Developer Certificate of Origin](https://developercertificate.org/).

## Reporting a title

Use the "Title report" issue form. Name the game only by its title ID from the manifest (`MicrosoftGame.config` or `AppxManifest.xml`), never by its name.

## Writing

Plain and short. State facts; do not restate what code or tables already say.
