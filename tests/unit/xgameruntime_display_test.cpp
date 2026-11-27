// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "display.hpp"

#include <cstddef>
#include <cstdint>

namespace {

namespace ds = g6shim::xgameruntime::display;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;

using Hdr = std::uint32_t (*)(Object*, std::uint32_t, void*);
using Acquire = HResult (*)(Object*, void**);
using Close = void (*)(Object*, void*);

template <class Function>
Function at(const Object& object, std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(object, offset);
}

Object* self(const Object& object) { return const_cast<Object*>(&object); }

}  // namespace

TEST(xgameruntime_display_sdr_and_display_deferrals) {
    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&ds::kHdrProvider, &ds::kHdrIid, &served) ==
          g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&ds::kHdrObject));

    CHECK(at<Hdr>(ds::kHdrObject, 0x20)(self(ds::kHdrObject), 0, nullptr) == ds::kHdrDisabled);

    void* handle = nullptr;
    CHECK(at<Acquire>(ds::kShellObject, 0x20)(self(ds::kShellObject), &handle) == g6shim::xgameruntime::kOk);
    CHECK(handle != nullptr);
    if (handle != nullptr) at<Close>(ds::kShellObject, 0x28)(self(ds::kShellObject), handle);
}
