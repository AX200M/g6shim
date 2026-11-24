// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/fence.hpp"
#include "d3d12_xs/object.hpp"

#include <windows.h>

using namespace g6shim::d3d12_xs;

TEST(d3d12_xs_signals_set_waiting_events) {
    Object* fence = nullptr;
    CHECK(fence::create(nullptr, 1, 0, &fence::kIids[0], &fence) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence, 0x40)(fence) == 1);
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event != nullptr);
    const auto wait = slot<HRESULT (*)(Object*, std::uint64_t, void*)>(fence, 0x48);
    const auto signal = slot<HRESULT (*)(Object*, std::uint64_t)>(fence, 0x50);
    CHECK(wait(fence, 5, event) == S_OK);
    CHECK(::WaitForSingleObject(event, 0) != WAIT_OBJECT_0);
    CHECK(signal(fence, 5) == S_OK);
    CHECK(::WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(wait(fence, 5, event) == S_OK);
    CHECK(::WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(wait(fence, 9, nullptr) == E_NOTIMPL);
    const auto at = slot<std::uint64_t (*)(Object*)>(fence, 0x60)(fence);
    CHECK(*reinterpret_cast<const std::uint64_t*>(at) == 5);
    ::CloseHandle(event);
    CHECK(slot<std::uint32_t (*)(Object*)>(fence, 0x10)(fence) == 0);
}
