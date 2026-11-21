// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <guiddef.h>
#include <unknwn.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace g6shim::winrt {

inline constexpr std::size_t kInspectableSlots = 6;
inline constexpr std::size_t kMaxSlots = 64;
inline constexpr std::size_t kDelegateInvokeSlot = 3;
inline constexpr GUID kNoIid = {};

using Methods = std::initializer_list<std::pair<std::size_t, const void*>>;

template <class F>
const void* fn(F f) {
    return reinterpret_cast<const void*>(f);
}

template <class... A>
HRESULT vcall(void* object, std::size_t slot, A... args) {
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return (*reinterpret_cast<Fn* const*>(object))[slot](object, args...);
}

inline void release(void* object) {
    if (object) static_cast<IUnknown*>(object)->Release();
}

inline void* add_ref(void* object) {
    if (object) static_cast<IUnknown*>(object)->AddRef();
    return object;
}

std::string guid_text(const GUID& g);

const void* const* make_vtable(Methods methods);

void* make_object(const std::string& class_name, const GUID& iid, const void* const* vtable);

void* make_probe(const std::string& class_name, const GUID& iid);

void log_once(const std::string& key, const char* fmt, ...);

}  // namespace g6shim::winrt
