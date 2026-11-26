; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[6:7], 0x0
s_load_dwordx4 s[20:23], s[4:5], 0x20
v_mov_b32 v1, s0
v_mov_b32 v2, s1
v_mov_b32 v3, s2
s_waitcnt lgkmcnt(0)
image_gather4_l v[4:7], [v1, v2, v3], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
buffer_store_dword v4, off, s[20:23], 0
buffer_store_dword v5, off, s[20:23], 0 offset:4
buffer_store_dword v6, off, s[20:23], 0 offset:8
buffer_store_dword v7, off, s[20:23], 0 offset:12
s_endpgm
