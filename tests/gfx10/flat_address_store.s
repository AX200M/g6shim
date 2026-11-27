; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
v_mov_b32 v0, s0
v_mov_b32 v1, s1
global_load_dword v2, v[0:1], off
s_waitcnt vmcnt(0)
v_add_nc_u32 v2, 5, v2
global_store_dword v[0:1], v2, off offset:4
s_endpgm
