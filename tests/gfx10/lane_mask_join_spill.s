; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
v_mov_b32 v1, s0
v_mov_b32 v2, s1
v_mov_b32 v3, s2
v_mov_b32 v4, s3
v_mbcnt_lo_u32_b32 v5, -1, 0
v_cmp_lt_u32 s6, v5, s1
v_writelane_b32 v6, s6, 0
s_cmp_eq_u32 s0, 0
s_cbranch_scc1 join
v_readlane_b32 s6, v6, 0
join:
v_cndmask_b32 v0, 0, 1, s6
v_lshlrev_b32 v5, 5, v5
s_waitcnt lgkmcnt(0)
buffer_store_dword v0, v5, s[8:11], 0 offen
s_endpgm
