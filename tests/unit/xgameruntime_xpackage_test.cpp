// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "title.hpp"
#include "xasync.hpp"
#include "xpackage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

namespace xp = g6shim::xgameruntime::xpackage;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

using Id = HResult (*)(Object*, std::size_t, std::uint8_t*);
using Packaged = bool (*)(Object*);
using Find = HResult (*)(Object*, const char*, std::uint32_t, const void*, std::uint32_t*);
using ProgressMethod = void (*)(Object*, const void*, xp::Progress*);
using Mount = HResult (*)(Object*, const char*, AsyncBlock*);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(xp::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xp::kObject); }

}  // namespace

TEST(xgameruntime_package_the_installation_is_complete_and_alone) {
    g6shim::xgameruntime::Title title;
    title.id = 0x639AA8DD;
    g6shim::xgameruntime::set_title(title);

    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&xp::kProvider, &xp::kIid2026, &served) ==
          g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&xp::kObject));

    std::uint8_t id[33] = {};
    CHECK(at<Id>(0x18)(self(), sizeof id, id) == g6shim::xgameruntime::kOk);
    CHECK(std::string(reinterpret_cast<const char*>(id)) == xp::identifier());
    CHECK(xp::identifier() == "639AA8DD");
    CHECK(at<Packaged>(0x20)(self()));

    std::uint32_t availability = 9;
    const std::string ours = xp::identifier();
    const Find find = at<Find>(0x60);
    CHECK(find(self(), ours.c_str(), 0, nullptr, &availability) == g6shim::xgameruntime::kOk);
    CHECK(availability == 0);
    CHECK(find(self(), "dlc", 0, nullptr, &availability) == xp::kNoPackage);

    xp::Progress progress{};
    at<ProgressMethod>(0x38)(self(), xp::monitor(), &progress);
    CHECK(progress.completed && progress.launchable);
    CHECK(progress.installed_bytes == progress.total_bytes);

    AsyncBlock block{};
    CHECK(at<Mount>(0x128)(self(), "dlc", &block) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&block, true) == xp::kNoPackage);
    g6shim::xgameruntime::clear_title();
}
