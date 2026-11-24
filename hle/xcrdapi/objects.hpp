// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace g6shim::xcrdapi {

inline constexpr HRESULT kSucceeded = 0;
inline constexpr HRESULT kNoInterface = static_cast<HRESULT>(0x80004002u);
inline constexpr HRESULT kInvalidArgument = static_cast<HRESULT>(0x80070057u);
inline constexpr HRESULT kOutOfMemory = static_cast<HRESULT>(0x8007000Eu);
inline constexpr HRESULT kNotImplemented = static_cast<HRESULT>(0x80004001u);
inline constexpr HRESULT kPending = static_cast<HRESULT>(0x8000000Au);

struct Guid {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::array<std::uint8_t, 8> data4;
};

constexpr bool operator==(const Guid& a, const Guid& b) {
    return a.data1 == b.data1 && a.data2 == b.data2 && a.data3 == b.data3 && a.data4 == b.data4;
}

std::string guid_text(const Guid* iid);

struct Object {
    const void* const* methods;
    const char* area;
};

template <typename T>
struct Instance {
    Object base;
    std::atomic<std::uint32_t> refs{1};
    T state;

    template <typename... Args>
    explicit Instance(Object object, Args&&... args)
        : base(object), state(std::forward<Args>(args)...) {}
};

template <typename T, typename... Args>
const void* create_instance(Object base, Args&&... args) {
    return new Instance<T>(base, std::forward<Args>(args)...);
}

template <typename T>
std::uint32_t add_ref_instance(Instance<T>* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename T>
std::uint32_t release_instance(Instance<T>* self) {
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

template <typename T>
HRESULT put(T* out, T value) {
    if (out == nullptr) return kInvalidArgument;
    *out = value;
    return kSucceeded;
}

template <std::size_t Slot>
HRESULT WINAPI missing(const Object* self) {
    G6SHIM_WARN("%s: method at 0x%zX is not implemented", self->area, Slot * sizeof(void*));
    return kNotImplemented;
}

template <typename F>
F method(void* object, std::size_t slot) {
    const auto* table = *static_cast<const void* const* const*>(object);
    return reinterpret_cast<F>(table[slot / sizeof(void*)]);
}

}  // namespace g6shim::xcrdapi
