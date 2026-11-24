# CPU instructions

The console's CPU accepts instructions some PC CPUs do not. A vectored exception handler in `g6shim_runtime.dll` recognises the encodings below when one raises an illegal-instruction exception, emulates the instruction and resumes after it. Other exceptions and other instructions continue the search unchanged.

| Bytes | Instruction | Emulation |
|---|---|---|
| `0f 01 fc` | `clzero` | writes zero to the 64-byte line holding `rax`; resumes after 3 bytes |
| `0f 01 fa` | `monitorx` | does nothing; resumes after 3 bytes |
| `0f 01 fb` | `mwaitx` | yields the thread; resumes after 3 bytes |
| `66 [rex] 0f ae /6` | `clwb m8` | resumes after the instruction; the PC's memory is already coherent |
| `66 [rex] 0f 78 /0 ib ib` | `extrq xmm, imm8, imm8` | first immediate is the length, second the index; low 64 bits of `xmm` become bits `index..index+length-1`; length 0 is 64 |
| `66 [rex] 0f 79 /r` | `extrq xmm1, xmm2` | as `extrq` with length in bits 0-5 and index in bits 8-13 of `xmm2`'s low half |
| `f2 [rex] 0f 78 /r ib ib` | `insertq xmm1, xmm2, imm8, imm8` | first immediate is the length, second the index; bits `index..index+length-1` of `xmm1` become the low bits of `xmm2`; length 0 is 64 |
| `f2 [rex] 0f 79 /r` | `insertq xmm1, xmm2` | as `insertq` with length in bits 0-5 and index in bits 8-13 of `xmm2`'s high half |

## Write-back loop

`clwb [base]` followed by `add base, 0x40`, `cmp base, limit` and `jbe` back to the `clwb` is retired in one exception. `base` ends past `limit` as the last pass leaves it, the compare flags (CF, PF, AF, ZF, SF, OF) are set from that final compare, and execution resumes after the `jbe`. Other `clwb` loops take one exception per cache line.

Instruction bytes are read only from the faulting instruction's page.
