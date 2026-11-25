// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace g6shim::xgameruntime {

namespace {

std::mutex g_queried_mutex;
std::vector<std::pair<Guid, Guid>> g_queried;

std::array<std::span<const Interface>, 12> interface_sets() {
    return {xasync_interfaces(),  xuser_interfaces(),      xsystem_interfaces(),  xgame_interfaces(),
            storage_interfaces(), xnetworking_interfaces(), xpackage_interfaces(), display_interfaces(),
            xgamesave_interfaces(), xstore_interfaces(),   xgameui_interfaces(),  speech_interfaces()};
}

const Interface* find_interface(const Guid& provider, const Guid& iid) {
    for (const std::span<const Interface> area : interface_sets())
        for (const Interface& entry : area)
            if (entry.provider == provider && entry.iid == iid) return &entry;
    return nullptr;
}

bool already_logged(const Guid& provider, const Guid& iid) {
    const auto found = std::find(g_queried.begin(), g_queried.end(), std::pair{provider, iid});
    if (found != g_queried.end()) return true;
    g_queried.emplace_back(provider, iid);
    return false;
}

}  // namespace

std::string guid_text(const Guid& value) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", value.data1,
                  value.data2, value.data3, value.data4[0], value.data4[1], value.data4[2], value.data4[3],
                  value.data4[4], value.data4[5], value.data4[6], value.data4[7]);
    return buffer;
}

std::uint32_t add_ref(Object*) { return 1; }

HResult missing_slot(const char* area, std::size_t slot) {
    G6SHIM_WARN("xgameruntime: %s method at 0x%zx not implemented", area, slot * sizeof(void*));
    return kNotImpl;
}

std::uint64_t token() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::optional<std::string> text(const char* value) {
    if (value == nullptr) return std::nullopt;
    return std::string(value);
}

HResult put_string(std::string_view value, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    const std::size_t length = value.size() + 1;
    if (buffer == nullptr || size < length) return kNotSufficientBuffer;
    std::memcpy(buffer, value.data(), value.size());
    buffer[value.size()] = 0;
    if (used != nullptr) *used = length;
    return kOk;
}

HResult query_api(const Guid* provider, const Guid* iid, void** out) {
    if (provider == nullptr || iid == nullptr) return kNotSupported;
    if (const Interface* found = find_interface(*provider, *iid)) {
        if (out != nullptr) {
            *out = const_cast<Object*>(found->object);
            return kOk;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_queried_mutex);
        if (!already_logged(*provider, *iid))
            G6SHIM_WARN("xgameruntime: runtime API area not implemented: provider=%s interface=%s",
                        guid_text(*provider).c_str(), guid_text(*iid).c_str());
    }
    return kNotSupported;
}

}  // namespace g6shim::xgameruntime

extern "C" std::int32_t WINAPI g6shim_XGameRuntimeInitialize() {
    G6SHIM_INFO("xgameruntime: XGameRuntimeInitialize");
    return g6shim::xgameruntime::kOk;
}

extern "C" void WINAPI g6shim_XGameRuntimeUninitialize() { G6SHIM_INFO("xgameruntime: XGameRuntimeUninitialize"); }

extern "C" std::int32_t WINAPI g6shim_InitializeApiImpl(std::uint64_t version, std::uint64_t build) {
    G6SHIM_INFO("xgameruntime: InitializeApiImpl version=%llu build=%llu",
                static_cast<unsigned long long>(version), static_cast<unsigned long long>(build));
    return g6shim::xgameruntime::kOk;
}

extern "C" std::int32_t WINAPI g6shim_InitializeApiImplEx(std::uint64_t version, std::uint64_t build,
                                                          std::uint64_t flags) {
    G6SHIM_INFO("xgameruntime: InitializeApiImplEx version=%llu build=%llu flags=%llu",
                static_cast<unsigned long long>(version), static_cast<unsigned long long>(build),
                static_cast<unsigned long long>(flags));
    return g6shim::xgameruntime::kOk;
}

extern "C" std::int32_t WINAPI g6shim_InitializeApiImplEx2(std::uint64_t version, std::uint64_t build,
                                                           std::uint64_t flags, const void* options) {
    G6SHIM_INFO("xgameruntime: InitializeApiImplEx2 version=%llu build=%llu flags=%llu options=%p",
                static_cast<unsigned long long>(version), static_cast<unsigned long long>(build),
                static_cast<unsigned long long>(flags), options);
    return g6shim::xgameruntime::kOk;
}

extern "C" void WINAPI g6shim_UninitializeApiImpl() { G6SHIM_INFO("xgameruntime: UninitializeApiImpl"); }

extern "C" std::int32_t WINAPI g6shim_QueryApiImpl(const g6shim::xgameruntime::Guid* provider,
                                                   const g6shim::xgameruntime::Guid* iid, void** out) {
    return g6shim::xgameruntime::query_api(provider, iid, out);
}

extern "C" void WINAPI g6shim_XErrorReport(std::int32_t result, const char* message) {
    const std::string detail = message != nullptr ? std::string(message) : std::string();
    G6SHIM_WARN("xgameruntime: XErrorReport hr=0x%08x message=%s", static_cast<std::uint32_t>(result),
                detail.c_str());
}
