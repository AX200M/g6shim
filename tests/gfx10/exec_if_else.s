; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
v_mov_b32 v1, s0
v_mov_b32 v2, s1
v_mov_b32 v3, s2
v_mov_b32 v4, s3
v_mov_b32 v0, v3
v_cmp_gt_u32 vcc_lo, v1, v2
s_and_saveexec_b32 s6, vcc_lo
s_cbranch_execz skip
v_mov_b32 v0, v4
skip:
s_or_b32 exec_lo, exec_lo, s6
s_waitcnt lgkmcnt(0)
buffer_store_dword v0, off, s[8:11], 0
s_endpgm
