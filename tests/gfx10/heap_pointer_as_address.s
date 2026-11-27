; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[8:11], s[6:7], 0x0
s_add_u32 s0, s4, 1
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, s0
buffer_store_dword v0, off, s[8:11], 0
s_endpgm
