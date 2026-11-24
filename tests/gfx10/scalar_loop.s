; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
v_mov_b32 v1, s0
v_mov_b32 v2, s1
v_mov_b32 v3, s2
v_mov_b32 v4, s3
v_mov_b32 v0, 0
s_mov_b32 s6, 4
loop:
v_add_nc_u32 v0, v0, v1
s_sub_u32 s6, s6, 1
s_cmp_lg_u32 s6, 0
s_cbranch_scc1 loop
s_waitcnt lgkmcnt(0)
buffer_store_dword v0, off, s[8:11], 0
s_endpgm
