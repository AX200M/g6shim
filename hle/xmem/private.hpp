// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

extern "C" BOOL WINAPI g6shim_xmem_map_alias(std::uintptr_t to, std::uintptr_t from, std::size_t bytes);

namespace g6shim::xmem::private_api {

using MapAlias = BOOL(WINAPI*)(std::uintptr_t to, std::uintptr_t from, std::size_t bytes);

struct Exports {
    MapAlias map_alias = nullptr;

    explicit operator bool() const { return map_alias != nullptr; }
};

inline FARPROC find_symbol(const char* name) {
    HMODULE module = ::GetModuleHandleW(L"xmem.dll");
    if (module == nullptr) module = ::GetModuleHandleW(nullptr);
    if (module == nullptr) return nullptr;
    return ::GetProcAddress(module, name);
}

inline Exports resolve() {
    Exports exports;
    exports.map_alias = reinterpret_cast<MapAlias>(find_symbol("g6shim_xmem_map_alias"));
    return exports;
}

}  // namespace g6shim::xmem::private_api
