// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs {

using Iid = std::array<std::uint8_t, 16>;

inline constexpr HRESULT kNotFound = static_cast<HRESULT>(0x887A0002u);
inline constexpr HRESULT kMoreData = static_cast<HRESULT>(0x887A0003u);

constexpr Iid guid(std::uint32_t a, std::uint16_t b, std::uint16_t c, std::array<std::uint8_t, 8> d) {
    return Iid{static_cast<std::uint8_t>(a),
               static_cast<std::uint8_t>(a >> 8),
               static_cast<std::uint8_t>(a >> 16),
               static_cast<std::uint8_t>(a >> 24),
               static_cast<std::uint8_t>(b),
               static_cast<std::uint8_t>(b >> 8),
               static_cast<std::uint8_t>(c),
               static_cast<std::uint8_t>(c >> 8),
               d[0],
               d[1],
               d[2],
               d[3],
               d[4],
               d[5],
               d[6],
               d[7]};
}

std::string guid_text(const Iid& id);

inline constexpr std::array<Iid, 3> kPageableIids{
    guid(0x63ee58fb, 0x1268, 0x4835, {0x86, 0xda, 0xf0, 0x08, 0xce, 0x62, 0xf0, 0xd6}),
    guid(0x905db94b, 0xa00c, 0x4140, {0x9d, 0xf5, 0x2b, 0x64, 0xca, 0x9e, 0xa3, 0x57}),
    guid(0xc4fec28f, 0x7966, 0x4e95, {0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8}),
};

struct Object {
    explicit Object(const void* const* table) noexcept : functions(table) {}
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    const void* const* functions;
    std::uint32_t bits = 0;
    std::atomic<std::uint32_t> refs{0};
};
static_assert(sizeof(Object) == 0x10);

std::uint32_t add_ref(Object* object) noexcept;
std::uint32_t release(Object* object) noexcept;

HRESULT answer(std::size_t kind, const Iid* iid, Object* found, Object** out);
bool is_iid(const Iid* iid, std::span<const Iid> known);

HRESULT get_private_data(Object* object, const Iid* id, std::uint32_t* size, std::uint8_t* data);
HRESULT set_private_data(Object* object, const Iid* id, std::uint32_t size, const std::uint8_t* data);
HRESULT set_name(Object* object, const wchar_t* name);
HRESULT parent_device(Object* object, const Iid* iid, Object** out);
HRESULT ignore(Object* object);
void forget_private_data(Object* object);

inline constexpr std::size_t kDevice = 0;
inline constexpr std::size_t kDxgiDevice = 1;
inline constexpr std::size_t kAdapter = 2;
inline constexpr std::size_t kFactory = 3;
inline constexpr std::size_t kOutput = 4;
inline constexpr std::size_t kHeap = 5;
inline constexpr std::size_t kResource = 6;
inline constexpr std::size_t kFence = 7;
inline constexpr std::size_t kQueue = 8;
inline constexpr std::size_t kAllocator = 9;
inline constexpr std::size_t kList = 10;
inline constexpr std::size_t kDescriptorHeap = 11;
inline constexpr std::size_t kRootSignature = 12;
inline constexpr std::size_t kQueryHeap = 13;
inline constexpr std::size_t kCommandSignature = 14;
inline constexpr std::size_t kPipeline = 15;
inline constexpr std::size_t kKindCount = 16;
inline constexpr std::size_t kMaxSlots = 160;

HRESULT report_missing(std::size_t kind, std::size_t slot);

template <std::size_t Kind, std::size_t Slot>
HRESULT missing_method() {
    return report_missing(Kind, Slot);
}

template <typename F>
F slot(const Object* object, std::size_t offset) {
    return reinterpret_cast<F>(object->functions[offset / 8]);
}

template <std::size_t Kind, std::size_t N, std::size_t... Index>
std::array<const void*, N> fill_missing(std::index_sequence<Index...>) {
    return std::array<const void*, N>{reinterpret_cast<const void*>(&missing_method<Kind, Index>)...};
}

template <std::size_t Kind, std::size_t N>
std::array<const void*, N> missing_table() {
    std::array<const void*, N> table = fill_missing<Kind, N>(std::make_index_sequence<N>{});
    table[1] = reinterpret_cast<const void*>(&add_ref);
    table[2] = reinterpret_cast<const void*>(&release);
    return table;
}

template <std::size_t N>
std::array<const void*, N> d3d12_object(std::array<const void*, N> table) {
    table[3] = reinterpret_cast<const void*>(&get_private_data);
    table[4] = reinterpret_cast<const void*>(&set_private_data);
    table[6] = reinterpret_cast<const void*>(&set_name);
    return table;
}

template <typename T>
std::uint32_t release_boxed(Object* object) {
    std::uint32_t current = object->refs.load(std::memory_order_acquire);
    while (current != 0) {
        if (object->refs.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
                                               std::memory_order_acquire))
            break;
    }
    if (current == 0) return 0;
    if (current == 1) {
        forget_private_data(object);
        delete static_cast<T*>(object);
        return 0;
    }
    return current - 1;
}

template <typename T>
HRESULT hand_out(std::size_t kind, std::unique_ptr<T> object, const Iid* iid, std::span<const Iid> known,
                 Object** out) {
    if (!is_iid(iid, known)) return answer(kind, iid, nullptr, out);
    if (out == nullptr) return S_FALSE;
    return answer(kind, iid, static_cast<Object*>(object.release()), out);
}

}  // namespace g6shim::d3d12_xs
