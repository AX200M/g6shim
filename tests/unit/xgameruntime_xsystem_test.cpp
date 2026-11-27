// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xsystem.hpp"

#include <cstdint>
#include <cstring>

namespace {

namespace xs = g6shim::xgameruntime::xsystem;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;

using Get = HResult (*)(Object*, std::size_t, std::uint8_t*, std::size_t*);
using Analytics = xs::AnalyticsInfo* (*)(Object*, xs::AnalyticsInfo*);

template <class Function>
Function at(const Object& object, std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(object, offset);
}

Object* self(const Object& object) { return const_cast<Object*>(&object); }

}  // namespace

TEST(xgameruntime_xsystem_identity_strings_fit_the_documented_sizes) {
    std::uint8_t buffer[64] = {};
    std::size_t used = 0;
    const std::size_t slots[] = {0x18, 0x20, 0x28};
    const std::size_t sizes[] = {39, 16, 45};
    for (std::size_t index = 0; index < 3; ++index) {
        const Get get = at<Get>(xs::kObject, slots[index]);
        CHECK(get(self(xs::kObject), sizes[index], buffer, &used) == g6shim::xgameruntime::kOk);
        CHECK(used <= sizes[index] && buffer[used - 1] == 0);
    }
    CHECK(std::memcmp(buffer, "VkljdG9", 7) == 0);
}

TEST(xgameruntime_xsystem_analytics_info_comes_back_through_the_buffer) {
    xs::AnalyticsInfo info{};
    const Analytics get = at<Analytics>(xs::kAnalyticsObject, 0x18);
    const xs::AnalyticsInfo* returned = get(self(xs::kAnalyticsObject), &info);
    CHECK(returned == &info);
    CHECK(sizeof(xs::AnalyticsInfo) == 0x90);
    CHECK(info.os.build == 22000);
    CHECK(std::memcmp(info.form.data(), "Xbox Series X\0", 14) == 0);
    CHECK(std::memcmp(info.family.data(), "Xbox\0", 5) == 0);
    CHECK(info.hosting_os.major == 10);
}
