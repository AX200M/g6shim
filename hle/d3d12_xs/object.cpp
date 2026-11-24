// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "object.hpp"

#include "device.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace g6shim::d3d12_xs {

namespace {

constexpr std::array<const char*, kKindCount> kNames{"device",
                                                     "DXGI device",
                                                     "adapter",
                                                     "factory",
                                                     "output",
                                                     "heap",
                                                     "resource",
                                                     "fence",
                                                     "command queue",
                                                     "command allocator",
                                                     "command list",
                                                     "descriptor heap",
                                                     "root signature",
                                                     "query heap",
                                                     "command signature",
                                                     "pipeline state"};

constexpr std::size_t kMaximumNameUnits = 2048;
constexpr std::size_t kUtf8PerUnit = 3;

std::array<std::array<std::atomic<bool>, kMaxSlots>, kKindCount> g_reported{};

using PrivateKey = std::pair<std::uintptr_t, Iid>;
std::map<PrivateKey, std::vector<std::uint8_t>> g_private;
std::mutex g_private_mutex;

std::mutex& private_mutex() { return g_private_mutex; }
std::map<PrivateKey, std::vector<std::uint8_t>>& private_data() { return g_private; }

std::string caller_offset() {
    void* frames[1] = {};
    if (::RtlCaptureStackBackTrace(2, 1, frames, nullptr) != 1) return {};
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(frames[0]), &module) == 0)
        return {};
    const auto at = reinterpret_cast<std::uintptr_t>(frames[0]);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    char text[32] = {};
    std::snprintf(text, sizeof text, "+0x%llx", static_cast<unsigned long long>(at - base));
    return text;
}

const char* kind_name(std::size_t kind) { return kind < kKindCount ? kNames[kind] : "object"; }

}  // namespace

std::string guid_text(const Iid& id) {
    const std::uint32_t a = static_cast<std::uint32_t>(id[0]) | static_cast<std::uint32_t>(id[1]) << 8 |
                            static_cast<std::uint32_t>(id[2]) << 16 | static_cast<std::uint32_t>(id[3]) << 24;
    const std::uint32_t b = static_cast<std::uint32_t>(id[4]) | static_cast<std::uint32_t>(id[5]) << 8;
    const std::uint32_t c = static_cast<std::uint32_t>(id[6]) | static_cast<std::uint32_t>(id[7]) << 8;
    char text[40] = {};
    std::snprintf(text,
                  sizeof text,
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  a,
                  b,
                  c,
                  id[8],
                  id[9],
                  id[10],
                  id[11],
                  id[12],
                  id[13],
                  id[14],
                  id[15]);
    return text;
}

std::uint32_t add_ref(Object* object) noexcept {
    return object->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t release(Object* object) noexcept {
    std::uint32_t current = object->refs.load(std::memory_order_relaxed);
    while (current != 0 && !object->refs.compare_exchange_weak(current, current - 1, std::memory_order_relaxed,
                                                               std::memory_order_relaxed)) {}
    return object->refs.load(std::memory_order_relaxed);
}

bool is_iid(const Iid* iid, std::span<const Iid> known) {
    if (iid == nullptr) return false;
    return std::find(known.begin(), known.end(), *iid) != known.end();
}

HRESULT answer(std::size_t kind, const Iid* iid, Object* found, Object** out) {
    if (out == nullptr) return E_POINTER;
    if (found != nullptr) {
        add_ref(found);
        *out = found;
        return S_OK;
    }
    const std::string text = iid != nullptr ? guid_text(*iid) : std::string{};
    G6SHIM_WARN("d3d12_xs: %s asked for unknown interface %s", kind_name(kind), text.c_str());
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT get_private_data(Object* object, const Iid* id, std::uint32_t* size, std::uint8_t* data) {
    if (id == nullptr || size == nullptr) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(private_mutex());
    auto& map = private_data();
    const auto entry = map.find(PrivateKey{reinterpret_cast<std::uintptr_t>(object), *id});
    if (entry == map.end()) {
        *size = 0;
        return kNotFound;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(entry->second.size());
    const std::uint32_t room = *size;
    *size = length;
    if (data == nullptr) return S_OK;
    if (room < length) return kMoreData;
    if (length != 0) std::memcpy(data, entry->second.data(), length);
    return S_OK;
}

HRESULT set_private_data(Object* object, const Iid* id, std::uint32_t size, const std::uint8_t* data) {
    if (id == nullptr) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(private_mutex());
    auto& map = private_data();
    const PrivateKey key{reinterpret_cast<std::uintptr_t>(object), *id};
    if (data == nullptr) {
        map.erase(key);
    } else {
        map[key] = std::vector<std::uint8_t>(data, data + size);
    }
    return S_OK;
}

void forget_private_data(Object* object) {
    const auto at = reinterpret_cast<std::uintptr_t>(object);
    std::lock_guard<std::mutex> lock(private_mutex());
    auto& map = private_data();
    for (auto it = map.begin(); it != map.end();) {
        if (it->first.first == at)
            it = map.erase(it);
        else
            ++it;
    }
}

HRESULT set_name(Object* object, const wchar_t* name) {
    std::string text;
    if (name != nullptr) {
        std::size_t units = 0;
        while (units < kMaximumNameUnits && name[units] != L'\0') ++units;
        text.resize(units * kUtf8PerUnit);
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(units), text.data(),
                                                  static_cast<int>(text.size()), nullptr, nullptr);
        text.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    }
    G6SHIM_TRACE("d3d12_xs: object %p named %s", static_cast<void*>(object), text.c_str());
    return S_OK;
}

HRESULT parent_device(Object*, const Iid* iid, Object** out) {
    const bool known = is_iid(iid, std::span<const Iid>(&device::kIid, 1));
    return answer(kDevice, iid, known ? device::device() : nullptr, out);
}

HRESULT ignore(Object*) { return S_OK; }

HRESULT report_missing(std::size_t kind, std::size_t slot) {
    if (kind < kKindCount && slot < kMaxSlots) {
        if (!g_reported[kind][slot].exchange(true, std::memory_order_relaxed)) {
            const std::string caller = caller_offset();
            G6SHIM_WARN("d3d12_xs: %s method at 0x%zx not implemented (caller %s)", kind_name(kind),
                        slot * 8, caller.empty() ? "unknown" : caller.c_str());
        }
    }
    return E_NOTIMPL;
}

}  // namespace g6shim::d3d12_xs
