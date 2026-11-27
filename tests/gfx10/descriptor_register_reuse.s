; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[4:5], 0x20
v_mov_b32 v2, s0
v_mov_b32 v3, s1
s_waitcnt lgkmcnt(0)
image_load v1, v[2:3], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_cmp_eq_u32 s0, 0
s_cbranch_scc1 join
s_mov_b32 s13, 7
join:
v_cvt_f32_ubyte0 v0, s13
s_waitcnt vmcnt(0)
buffer_store_dword v0, off, s[16:19], 0
buffer_store_dword v1, off, s[16:19], 0 offset:4
s_endpgm
