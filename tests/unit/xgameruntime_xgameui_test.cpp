// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xasync.hpp"
#include "xgameui.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace {

namespace xu = g6shim::xgameruntime::xgameui;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using g6shim::xgameruntime::xasync::AsyncBlock;

using Card = HResult (*)(Object*, AsyncBlock*, const void*, std::uint64_t);
using Done = HResult (*)(Object*, AsyncBlock*);
using Entry = HResult (*)(Object*, AsyncBlock*, const char*, const char*, const char*, std::uint32_t,
                          std::uint32_t);
using Size = HResult (*)(Object*, AsyncBlock*, std::uint32_t*);
using Value = HResult (*)(Object*, AsyncBlock*, std::uint32_t, char*, std::uint32_t*);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(xu::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xu::kObject); }

std::optional<std::string> typed_or_cancelled(const std::string& title, const std::string&,
                                              const std::string& initial, std::uint32_t) {
    if (title.rfind("Type", 0) != 0) return std::nullopt;
    return initial + "typed";
}

}  // namespace

TEST(xgameruntime_gameui_dialogs_end_with_what_was_typed) {
    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&xu::kProvider, &xu::kIid2026, &served) ==
          g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&xu::kObject));

    xu::ask() = &typed_or_cancelled;

    AsyncBlock block{};
    CHECK(at<Card>(0x38)(self(), &block, nullptr, 1) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    CHECK(at<Done>(0x40)(self(), &block) == g6shim::xgameruntime::kOk);

    const Entry entry = at<Entry>(0x88);
    AsyncBlock dismissed{};
    CHECK(entry(self(), &dismissed, "Name", nullptr, nullptr, 0, 16) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&dismissed, true) == g6shim::xgameruntime::kAbort);
    std::uint32_t size = 0;
    CHECK(at<Size>(0x90)(self(), &dismissed, &size) == g6shim::xgameruntime::kAbort);

    AsyncBlock typed{};
    CHECK(entry(self(), &typed, "Type", nullptr, "abc", 0, 6) == g6shim::xgameruntime::kOk);
    CHECK(g6shim::xgameruntime::xasync::XAsyncGetStatus(&typed, true) == g6shim::xgameruntime::kOk);
    CHECK(at<Size>(0x90)(self(), &typed, &size) == g6shim::xgameruntime::kOk);
    CHECK(size == 7);
    char out[7] = {};
    std::uint32_t used = 0;
    CHECK(at<Value>(0x98)(self(), &typed, 7, out, &used) == g6shim::xgameruntime::kOk);
    CHECK(std::string(out) == "abctyp");
    CHECK(used == 7);

    CHECK(xu::limit_text("\xc3\xa9t\xc3\xa9s", 3) == "\xc3\xa9t\xc3\xa9");
    CHECK(xu::limit_text("abc", 0) == "abc");
}
