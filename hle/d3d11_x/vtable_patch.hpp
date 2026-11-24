// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>

namespace g6shim::d3d {

template <class Fn>
Fn patch_vtable(void* object, std::size_t slot, Fn replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    void* const ours = reinterpret_cast<void*>(replacement);
    if (vtable[slot] == ours) return nullptr;
    Fn original = reinterpret_cast<Fn>(vtable[slot]);
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    vtable[slot] = ours;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return original;
}

}  // namespace g6shim::d3d
