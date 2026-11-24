// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace g6shim::xgameruntime {

using HResult = std::int32_t;

inline constexpr HResult kOk = 0;
inline constexpr HResult kNotSupported = static_cast<HResult>(0x80070032u);
inline constexpr HResult kPending = static_cast<HResult>(0x8000000Au);
inline constexpr HResult kAbort = static_cast<HResult>(0x80004004u);
inline constexpr HResult kNotImpl = static_cast<HResult>(0x80004001u);
inline constexpr HResult kInvalidArg = static_cast<HResult>(0x80070057u);
inline constexpr HResult kNotSufficientBuffer = static_cast<HResult>(0x8007007Au);
inline constexpr HResult kIllegalMethodCall = static_cast<HResult>(0x8000000Eu);

struct Guid {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::array<std::uint8_t, 8> data4;

    friend bool operator==(const Guid&, const Guid&) = default;
};

constexpr Guid guid(std::uint32_t data1, std::uint16_t data2, std::uint16_t data3, std::uint8_t d0,
                    std::uint8_t d1, std::uint8_t d2, std::uint8_t d3, std::uint8_t d4, std::uint8_t d5,
                    std::uint8_t d6, std::uint8_t d7) {
    return Guid{data1, data2, data3, {d0, d1, d2, d3, d4, d5, d6, d7}};
}

std::string guid_text(const Guid& value);

struct Object {
    const void* const* vtable;
    const char* area;
};

struct Interface {
    Guid provider;
    Guid iid;
    const Object* object;
};

template <class Function>
const void* slot(Function function) {
    return reinterpret_cast<const void*>(function);
}

template <class Function>
Function method(const Object& object, std::size_t offset) {
    return (*reinterpret_cast<Function const* const*>(&object.vtable))[offset / sizeof(void*)];
}

std::uint32_t add_ref(Object*);

HResult missing_slot(const char* area, std::size_t slot);

template <std::size_t Slot>
HResult missing(Object* object) {
    return missing_slot(object != nullptr ? object->area : "", Slot);
}

std::uint64_t token();

std::optional<std::string> text(const char* value);

template <class T>
HResult put(T* out, T value) {
    if (out == nullptr) return kInvalidArg;
    *out = value;
    return kOk;
}

HResult put_string(std::string_view value, std::size_t size, std::uint8_t* buffer, std::size_t* used);

HResult query_api(const Guid* provider, const Guid* iid, void** out);

std::span<const Interface> xasync_interfaces();
std::span<const Interface> xuser_interfaces();
std::span<const Interface> xsystem_interfaces();
std::span<const Interface> xgame_interfaces();

}  // namespace g6shim::xgameruntime
