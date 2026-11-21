// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"
#include "mapping.hpp"

#include <g6shim/winrt/object.hpp>

#include <windows.h>
#include <inspectable.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace g6shim::winrt {

namespace {

struct ReadingObject {
    const void* const* vtable;
    std::atomic<ULONG> refs{1};
    input::Reading reading;
};

HRESULT __stdcall QueryInterface(ReadingObject* self, REFIID, void** out) {
    if (!out) return E_POINTER;
    ++self->refs;
    *out = self;
    return S_OK;
}

ULONG __stdcall AddRef(ReadingObject* self) { return ++self->refs; }

ULONG __stdcall Release(ReadingObject* self) {
    const ULONG n = --self->refs;
    if (n == 0) delete self;
    return n;
}

HRESULT __stdcall GetIids(ReadingObject*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

HRESULT __stdcall GetRuntimeClassName(ReadingObject*, HSTRING* name) {
    if (!name) return E_POINTER;
    *name = nullptr;
    return S_OK;
}

HRESULT __stdcall GetTrustLevel(ReadingObject*, TrustLevel* level) {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

HRESULT __stdcall get_Timestamp(ReadingObject* self, std::uint64_t* value) {
    if (!value) return E_POINTER;
    *value = self->reading.timestamp;
    return S_OK;
}

HRESULT __stdcall get_Buttons(ReadingObject* self, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = self->reading.buttons;
    return S_OK;
}

template <std::uint32_t Bit>
HRESULT __stdcall get_Pressed(ReadingObject* self, BYTE* value) {
    if (!value) return E_POINTER;
    *value = (self->reading.buttons & Bit) ? 1 : 0;
    return S_OK;
}

enum Axis : std::size_t { kLeftTrigger, kRightTrigger, kLeftX, kLeftY, kRightX, kRightY };

template <std::size_t Field>
HRESULT __stdcall get_Axis(ReadingObject* self, float* value) {
    if (!value) return E_POINTER;
    const float axes[] = {self->reading.left_trigger, self->reading.right_trigger, self->reading.left_x,
                          self->reading.left_y,       self->reading.right_x,       self->reading.right_y};
    *value = axes[Field];
    return S_OK;
}

HRESULT __stdcall Gamepad_GetCurrentReading(void*, void** reading) {
    using namespace input;
    static const void* const table[] = {
        fn(&QueryInterface),       fn(&AddRef),
        fn(&Release),              fn(&GetIids),
        fn(&GetRuntimeClassName),  fn(&GetTrustLevel),
        fn(&get_Timestamp),        fn(&get_Buttons),
        fn(&get_Pressed<kMenu>),   fn(&get_Pressed<kView>),
        fn(&get_Pressed<kA>),      fn(&get_Pressed<kB>),
        fn(&get_Pressed<kX>),      fn(&get_Pressed<kY>),
        fn(&get_Pressed<kDPadUp>), fn(&get_Pressed<kDPadDown>),
        fn(&get_Pressed<kDPadLeft>), fn(&get_Pressed<kDPadRight>),
        fn(&get_Pressed<kLeftShoulder>), fn(&get_Pressed<kRightShoulder>),
        fn(&get_Pressed<kLeftThumbstick>), fn(&get_Pressed<kRightThumbstick>),
        fn(&get_Axis<kLeftTrigger>), fn(&get_Axis<kRightTrigger>),
        fn(&get_Axis<kLeftX>),       fn(&get_Axis<kLeftY>),
        fn(&get_Axis<kRightX>),      fn(&get_Axis<kRightY>),
    };
    if (!reading) return E_POINTER;
    *reading = new ReadingObject{table, 1, read()};
    return S_OK;
}

}  // namespace

const void* gamepad_get_current_reading() { return fn(&Gamepad_GetCurrentReading); }

}  // namespace g6shim::winrt
