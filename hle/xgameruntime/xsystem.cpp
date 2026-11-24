// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xsystem.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace g6shim::xgameruntime::xsystem {

namespace {

using This = Object*;

std::array<char, 64> fixed_text(std::string_view value) {
    std::array<char, 64> out{};
    const std::size_t length = std::min(value.size(), out.size());
    std::memcpy(out.data(), value.data(), length);
    return out;
}

HResult console_id(This, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    return put_string(kConsoleId, size, buffer, used);
}

HResult sandbox_id(This, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    return put_string(kSandbox, size, buffer, used);
}

HResult device_id(This, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    return put_string(kDeviceId, size, buffer, used);
}

AnalyticsInfo* analytics_info(This, AnalyticsInfo* out) {
    if (out == nullptr) {
        G6SHIM_WARN("xgameruntime: analytics information requested without a buffer");
        return nullptr;
    }
    const AnalyticsInfo info{kOsVersion, kOsVersion, fixed_text(kFamily), fixed_text(kForm)};
    *out = info;
    return out;
}

const std::array<const void*, 6> kVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&console_id), slot(&sandbox_id), slot(&device_id)};

const std::array<const void*, 4> kAnalyticsVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&analytics_info)};

}  // namespace

const Object kObject{kVtable.data(), "system"};
const Object kAnalyticsObject{kAnalyticsVtable.data(), "analytics"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {
        {kProvider, kIid, &kObject},
        {kProvider, kIid2026, &kObject},
        {kAnalytics, kAnalytics, &kAnalyticsObject},
    };
    return entries;
}

}  // namespace g6shim::xgameruntime::xsystem

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xsystem_interfaces() {
    return xsystem::interfaces();
}
