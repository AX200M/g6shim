; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, s0
v_mov_b32 v1, s1
v_mov_b32 v2, s2
v_mov_b32 v3, s3
image_atomic_smax v[2:3], v[0:1], s[8:15] dmask:0x3 dim:SQ_RSRC_IMG_2D glc
s_endpgm
