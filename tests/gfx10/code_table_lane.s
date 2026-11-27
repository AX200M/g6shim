; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
s_getpc_b64 s[2:3]
s_add_u32 s2, s2, table - 16
s_addc_u32 s3, s3, 0
v_lshlrev_b32 v1, 2, v0
v_add_co_u32 v2, s12, v1, s2
v_add_co_ci_u32 v3, s12, 0, s3, s12
v_readfirstlane_b32 s12, v2
v_readfirstlane_b32 s13, v3
s_load_dword s6, s[12:13], 0x0
s_waitcnt lgkmcnt(0)
v_mov_b32 v4, s6
v_mov_b32 v5, 0
buffer_store_dword v4, v5, s[8:11], 0 offen
s_endpgm
table:
.long 0x11111111, 0x22222222, 0x33333333, 0x44444444
