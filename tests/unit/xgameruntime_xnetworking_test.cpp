// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xasync.hpp"
#include "xnetworking.hpp"

#include <cstddef>
#include <cstdint>

namespace {

namespace xn = g6shim::xgameruntime::xnetworking;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

using Hint = HResult (*)(Object*, xn::ConnectivityHint*);
using Begin = HResult (*)(Object*, AsyncBlock*);
using Port = HResult (*)(Object*, AsyncBlock*, std::uint16_t*);
using Url = HResult (*)(Object*, const char*, AsyncBlock*);
using Size = HResult (*)(Object*, AsyncBlock*, std::size_t*);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(xn::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xn::kObject); }

}  // namespace

TEST(xgameruntime_networking_is_offline_but_initialized) {
    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&xn::kProvider, &xn::kIid2026, &served) ==
          g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&xn::kObject));

    CHECK(sizeof(xn::ConnectivityHint) == 16);
    xn::ConnectivityHint hint{};
    CHECK(at<Hint>(0x78)(self(), &hint) == g6shim::xgameruntime::kOk);
    CHECK(hint.initialized);
    CHECK(hint.level == 1);
    CHECK(hint.cost == 0);

    AsyncBlock block{};
    CHECK(at<Begin>(0x20)(self(), &block) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    std::uint16_t port = 0;
    CHECK(at<Port>(0x28)(self(), &block, &port) == g6shim::xgameruntime::kOk);
    CHECK(port == 3074);

    AsyncBlock security{};
    CHECK(at<Url>(0x40)(self(), "https://example.invalid/", &security) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&security, true) == xn::kUnreachable);
    std::size_t size = 0;
    CHECK(at<Size>(0x48)(self(), &security, &size) == xn::kUnreachable);
}
