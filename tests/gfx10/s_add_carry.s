; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
v_mov_b32 v1, s0
v_mov_b32 v2, s1
v_mov_b32 v3, s2
v_mov_b32 v4, s3
s_add_u32 s6, s0, s1
s_addc_u32 s7, s2, s3
v_mov_b32 v5, s6
v_add_nc_u32 v0, s7, v5
s_waitcnt lgkmcnt(0)
buffer_store_dword v0, off, s[8:11], 0
s_endpgm
