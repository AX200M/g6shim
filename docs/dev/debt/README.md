# Technical-debt ledger

One file per module: `docs/dev/debt/<module>.md`. Gaps that belong to no module go in `project.md`.

Each file has four sections:

| Section | What goes there |
|---|---|
| Build | Toolchain, packaging and runtime dependencies |
| Silent stubs | Functions that report success without doing the work |
| Unknown function info | Signatures, argument meanings and return values taken from observation and not confirmed |
| Functional | Behaviour that differs from the console |

An entry is a list item. Continuation lines are indented by two spaces. Two optional trailers:

- `Closes when:` the measurement, test or title that would close the entry.
- `Title IDs:` the 8-digit hex title IDs where the gap was observed.

```markdown
- `XMemAlloc` ignores its attribute bits.
  Closes when: the attribute bits are decoded.
  Title IDs: 3DD7389B
```

## Fragments

Pull requests never edit the ledger files. They add `docs/dev/debt/changes/<branch>.md`:

```markdown
# kernelx

## add Silent stubs

- `SetThreadName` returns 0 without naming the thread.

## change `XMemAlloc` ignores

- `XMemAlloc` honours the alignment bits and ignores the others.

## remove `WaitOnAddress`
```

- `# <module>` names the ledger file.
- `## add <section>` appends one or more entries to that section.
- `## change <text>` replaces the one entry that contains `<text>`.
- `## remove <text>` deletes the one entry that contains `<text>`.

`python tools/debt_fold.py --check` validates the ledger and the fragments. `--show <module>` prints a ledger with the fragments applied. A plain run folds every fragment in and deletes it; the maintainer runs it when merging.
