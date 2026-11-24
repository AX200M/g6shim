# GFX10 translator fixtures

`tests/gfx10/*.s` are small hand-written GFX10 compute programs, one translator rule each. `tests/unit/gfx10_fixtures.inc` holds their assembled words. `shader_fixtures_translate_and_validate` lowers every program to DXIL and checks the container with the DXIL validator, so neither CI nor contributors need LLVM.

## Program shape

Every program has the same frame around its rule:

| Part | Instructions |
|---|---|
| inputs | `s[0:3]` root constants; `s4` the descriptor table address, `s5` set to 0 |
| descriptor | `s_load_dwordx4 s[8:11], s[4:5], 0x0`: the output UAV |
| operands | `v1`-`v4` copied from `s0`-`s3` |
| rule | the instructions under test, result in `v0` |
| output | `s_waitcnt lgkmcnt(0)`, `buffer_store_dword v0, off, s[8:11], 0`, `s_endpgm` |

The test lowers each program as a wave32 compute shader with 8 VGPRs, five user SGPRs (root constants at user data 0, the table at user data 4, one UAV in it) and a 1x1x1 group.

## Adding a program

1. Write `tests/gfx10/<rule>.s` in the frame above; the file name is the fixture's name.
2. Regenerate the words with an `llvm-mc` that has the AMDGPU target:

```bat
python tools/gfx10_fixtures.py --llvm-mc <path to llvm-mc.exe>
```

The script assembles each program with `-triple=amdgcn-amd-amdpal -mcpu=gfx1030`, takes the `.text` section of the object and writes the words in file-name order. `--check` exits 1 when the committed words are stale; `LLVM_MC` can name the tool instead of `--llvm-mc`.

3. Commit the `.s` file and the regenerated `.inc` together.
