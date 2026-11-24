// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <unknwn.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace g6shim::d3d {

inline constexpr std::size_t kProxySlots = 256;
inline constexpr std::size_t kProxyObjectSize = 0x1000;
inline constexpr std::size_t kProxyFirstField = 0x20;

using Override = std::pair<std::size_t, const void*>;

void* make_proxy(IUnknown* real, std::size_t base, const char* name, std::initializer_list<Override> overrides = {},
                 const std::vector<Override>& fields = {});

const void* forward_thunk(std::size_t slot);

IUnknown* real_object(void* proxy);

using Forward5 = HRESULT(STDMETHODCALLTYPE*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                             std::uintptr_t);

inline HRESULT forward5(std::size_t slot, void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c,
                        std::uintptr_t d, std::uintptr_t e) {
    return reinterpret_cast<Forward5>(forward_thunk(slot))(self, a, b, c, d, e);
}

}  // namespace g6shim::d3d
