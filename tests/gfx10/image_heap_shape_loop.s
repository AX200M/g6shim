; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[20:23], s[6:7], 0x0
s_lshl_b32 s2, s0, 5
v_mov_b32 v0, 0
loop:
s_load_dwordx8 s[12:19], s[4:5], s2
s_waitcnt lgkmcnt(0)
v_alignbit_b32 v0, s14, s13, 30
s_cbranch_execnz loop
buffer_store_dword v0, off, s[20:23], 0
s_endpgm
