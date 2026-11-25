// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xnetworking.hpp"

#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <array>

namespace g6shim::xgameruntime::xnetworking {

namespace {

using This = Object*;

constexpr ConnectivityHint kOffline{kOfflineLevel, 0, 0, true, false, false, false};

std::uint8_t kPort = 0;
std::uint8_t kSecurity = 0;

HResult udp_port(This, std::uint16_t* out) { return put(out, kUdpPort); }

HResult udp_port_async(This, xasync::AsyncBlock* block) {
    return xasync::finished_with(block, &kPort, kOk, xasync::bytes(kUdpPort));
}

HResult udp_port_result(This, xasync::AsyncBlock* block, std::uint16_t* out) {
    return xasync::result(block, &kPort, out);
}

HResult security_info_async(This, const char* url, xasync::AsyncBlock* block) {
    G6SHIM_TRACE("xgameruntime: security information for %s: offline", text(url).value_or("").c_str());
    return xasync::finished(block, &kSecurity, kUnreachable);
}

HResult security_info_size(This, xasync::AsyncBlock* block, std::size_t* out) {
    return xasync::XAsyncGetResultSize(block, out);
}

HResult security_info_result(This, xasync::AsyncBlock* block, std::size_t size, std::size_t* used,
                             std::uint8_t* buffer, const void**) {
    return xasync::XAsyncGetResult(block, &kSecurity, size, buffer, used);
}

HResult verify_certificate(This, void*, const void*) { return kUnreachable; }

HResult connectivity_hint(This, ConnectivityHint* out) { return put(out, kOffline); }

HResult register_change(This, const void*, void*, const void*, std::uint64_t* out) {
    return put(out, token());
}

bool unregister_change(This, std::uint64_t, bool) { return true; }

const std::array<const void*, 18> kVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&udp_port),
    slot(&udp_port_async),
    slot(&udp_port_result),
    slot(&missing<6>),
    slot(&missing<7>),
    slot(&security_info_async),
    slot(&security_info_size),
    slot(&security_info_result),
    slot(&missing<11>),
    slot(&missing<12>),
    slot(&missing<13>),
    slot(&verify_certificate),
    slot(&connectivity_hint),
    slot(&register_change),
    slot(&unregister_change),
};

}  // namespace

const Object kObject{kVtable.data(), "networking"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kProvider, &kObject}, {kProvider, kIid2026, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::xnetworking

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xnetworking_interfaces() {
    return xnetworking::interfaces();
}
