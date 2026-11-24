// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/winrt/events.hpp>

#include <windows.h>
#include <unknwn.h>

#include <atomic>

namespace {

using namespace g6shim::winrt;

struct Handler : IUnknown {
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

EventCallShape shape(bool object, bool writable, bool token, bool zero) {
    EventCallShape s;
    s.first_is_object = object;
    s.second_is_writable = writable;
    s.first_is_token = token;
    s.first_is_zero = zero;
    return s;
}

}  // namespace

TEST(auto_event_classification) {
    CHECK(classify_event_call(shape(true, true, false, false)) == EventCall::Add);
    CHECK(classify_event_call(shape(true, false, false, false)) == EventCall::None);
    CHECK(classify_event_call(shape(false, true, false, false)) == EventCall::None);
    CHECK(classify_event_call(shape(false, false, true, false)) == EventCall::Remove);
    CHECK(classify_event_call(shape(false, true, true, false)) == EventCall::Remove);
    CHECK(classify_event_call(shape(false, false, false, true)) == EventCall::RemoveNeverAdded);
    CHECK(classify_event_call(shape(false, true, false, true)) == EventCall::None);
    CHECK(classify_event_call(shape(false, false, false, false)) == EventCall::None);
}

TEST(auto_event_reads_real_arguments) {
    Handler handler;
    std::int64_t token = 0;
    const auto add = event_call_shape(reinterpret_cast<std::uintptr_t>(static_cast<IUnknown*>(&handler)),
                                      reinterpret_cast<std::uintptr_t>(&token));
    CHECK(add.first_is_object && add.second_is_writable && !add.first_is_zero);

    const auto plain = event_call_shape(5, 7);
    CHECK(!plain.first_is_object && !plain.second_is_writable && !plain.first_is_token);

    static const std::int64_t constant = 0;
    const auto read_only = event_call_shape(0, reinterpret_cast<std::uintptr_t>(&constant));
    CHECK(read_only.first_is_zero);
}

TEST(auto_event_adds_and_removes_handlers) {
    Handler handler;
    std::int64_t token = 0;
    long result = -1;
    int target = 0;
    const EventCall added = auto_event(&target, 3, reinterpret_cast<std::uintptr_t>(static_cast<IUnknown*>(&handler)),
                                       reinterpret_cast<std::uintptr_t>(&token), result);
    CHECK(added == EventCall::Add && result == S_OK);
    CHECK(token >= kFirstEventToken && is_event_token(token));
    CHECK(handler.refs == 2);

    const EventCall removed = auto_event(&target, 4, static_cast<std::uintptr_t>(token), 0, result);
    CHECK(removed == EventCall::Remove && result == S_OK);
    CHECK(!is_event_token(token) && handler.refs == 1);

    const EventCall never = auto_event(&target, 4, 0, 1, result);
    CHECK(never == EventCall::RemoveNeverAdded && result == S_OK);
    CHECK(auto_event(&target, 5, 42, 43, result) == EventCall::None);
}
