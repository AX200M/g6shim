// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "title.hpp"
#include "xasync.hpp"
#include "xstore.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

namespace xs = g6shim::xgameruntime::xstore;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

using Context = HResult (*)(Object*, const void*, const void**);
using Begin = HResult (*)(Object*, const void*, AsyncBlock*);
using Result = HResult (*)(Object*, AsyncBlock*, xs::GameLicense*);
using Updates = HResult (*)(Object*, const void*, AsyncBlock*);
using Count = HResult (*)(Object*, AsyncBlock*, std::uint32_t*);
using Durable = HResult (*)(Object*, const void*, const char*, AsyncBlock*);
using License = HResult (*)(Object*, AsyncBlock*, const void**);
using Valid = bool (*)(Object*, const void*);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(xs::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xs::kObject); }

}  // namespace

TEST(xgameruntime_store_the_game_is_licensed_and_the_store_is_offline) {
    CHECK(sizeof(xs::GameLicense) == 104);
    g6shim::xgameruntime::Title title;
    title.id = 0x639AA8DD;
    title.store_id = "9&C";
    g6shim::xgameruntime::set_title(title);

    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&xs::kProvider, &xs::kIid2026, &served) ==
          g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&xs::kObject));

    const void* context = nullptr;
    CHECK(at<Context>(0x18)(self(), nullptr, &context) == g6shim::xgameruntime::kOk);
    CHECK(context != nullptr);

    AsyncBlock block{};
    CHECK(at<Begin>(0xe0)(self(), context, &block) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    xs::GameLicense license{};
    CHECK(at<Result>(0xe8)(self(), &block, &license) == g6shim::xgameruntime::kOk);
    CHECK(license.is_active);
    CHECK(!license.is_trial);
    CHECK(std::string(license.sku_store_id.data()).substr(0, 7) == "9&C/001");

    AsyncBlock updates{};
    CHECK(at<Updates>(0x1b8)(self(), context, &updates) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&updates, true) == xs::kNetworkError);
    std::uint32_t count = 0;
    CHECK(at<Count>(0x1c0)(self(), &updates, &count) == xs::kNetworkError);

    AsyncBlock durable{};
    CHECK(at<Durable>(0x238)(self(), context, "9XYZ", &durable) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&durable, true) == g6shim::xgameruntime::kOk);
    const void* durable_license = nullptr;
    CHECK(at<License>(0x240)(self(), &durable, &durable_license) == g6shim::xgameruntime::kOk);
    CHECK(durable_license != nullptr);
    CHECK(!at<Valid>(0xb0)(self(), durable_license));
    g6shim::xgameruntime::clear_title();
}
