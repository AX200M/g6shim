; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_lshl_b32 s8, s0, 5
s_load_dwordx4 s[8:11], s[4:5], s8
s_mov_b32 s7, 0
s_load_dwordx4 s[12:15], s[6:7], 0
s_waitcnt lgkmcnt(0)
buffer_load_dword v0, v0, s[8:11], 0 offen
s_waitcnt vmcnt(0)
buffer_store_dword v0, off, s[12:15], 0
s_endpgm
