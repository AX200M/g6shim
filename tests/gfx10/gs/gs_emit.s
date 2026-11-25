; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s0, 3
s_bfm_b64 s[2:3], s0, 0
s_cselect_b64 exec, s[2:3], 0
v_mov_b32 v2, v0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_0
v_mov_b32 v3, v0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1
v_mov_b32 v4, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_0
ds_read_b32 v5, v2 offset:16
v_mov_b32 v6, 0xbf000000
v_mov_b32 v7, 0xbf000000
v_mov_b32 v8, 0
v_mov_b32 v9, 0x3f800000
ds_write2_b32 v2, v6, v7 offset0:0 offset1:1
ds_write2_b32 v2, v8, v9 offset0:2 offset1:3
ds_write_b32 v2, v5 offset:16
s_sendmsg 0x22
v_mov_b32 v6, 0x3f000000
ds_write2_b32 v2, v6, v7 offset0:0 offset1:1
ds_write2_b32 v2, v8, v9 offset0:2 offset1:3
ds_write_b32 v2, v5 offset:16
s_sendmsg 0x22
v_mov_b32 v6, 0
v_mov_b32 v7, 0x3f000000
ds_write2_b32 v2, v6, v7 offset0:0 offset1:1
ds_write2_b32 v2, v8, v9 offset0:2 offset1:3
ds_write_b32 v2, v5 offset:16
s_sendmsg 0x22
s_sendmsg 0x12
s_sendmsg 3
s_endpgm
