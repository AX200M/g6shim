; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[6:7], 0x0
s_load_dwordx4 s[20:23], s[4:5], 0x20
v_mov_b32 v0, 0x3c800000
v_mov_b32 v1, 0
v_mov_b32 v2, 0
v_mov_b32 v3, 0x3c800000
v_mov_b32 v4, 0x3f000000
v_mov_b32 v5, 0x3f000000
v_mov_b32 v6, 0x3f800000
s_waitcnt lgkmcnt(0)
image_sample_d_cl v7, v[0:6], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
image_sample_cl v0, v[4:6], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
buffer_store_dword v7, off, s[20:23], 0
buffer_store_dword v0, off, s[20:23], 0 offset:4
s_endpgm
