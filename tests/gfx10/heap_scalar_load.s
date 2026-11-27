; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_lshl_b32 s22, s0, 5
s_load_dwordx4 s[12:15], s[4:5], s22
s_mov_b32 s7, 0
s_load_dwordx4 s[16:19], s[6:7], 0x0
s_waitcnt lgkmcnt(0)
s_buffer_load_dwordx8 s[20:27], s[12:15], s1
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, s20
v_mov_b32 v1, s25
buffer_store_dword v0, off, s[16:19], 0
buffer_store_dword v1, off, s[16:19], 0 offset:4
s_endpgm
