// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <array>
#include <string>

namespace g6shim::xgameruntime::display {

namespace {

using This = Object*;

constexpr wchar_t kDeferralReason[] = L"Title display deferral";

HResult last_error() {
    const DWORD code = GetLastError();
    return static_cast<HResult>(0x80070000u | (code & 0xffffu));
}

std::uint32_t try_enable_hdr(This, std::uint32_t preference, void*) {
    G6SHIM_INFO("xgameruntime: HDR preference %u: disabled (SDR presentation)", preference);
    return kHdrDisabled;
}

HResult launch_uri(This, const void*, const char* uri) {
    G6SHIM_INFO("xgameruntime: URI not launched: %s", text(uri).value_or("").c_str());
    return kNotSupported;
}

HResult acquire_deferral(This, void** out) {
    if (out == nullptr) return kInvalidArg;
    std::wstring reason(kDeferralReason);
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason.data();
    const HANDLE request = PowerCreateRequest(&context);
    if (request == INVALID_HANDLE_VALUE) return last_error();
    if (PowerSetRequest(request, PowerRequestDisplayRequired) == 0) {
        const HResult result = last_error();
        CloseHandle(request);
        return result;
    }
    return put(out, static_cast<void*>(request));
}

void close_deferral(This, void* handle) {
    if (handle == nullptr) return;
    PowerClearRequest(handle, PowerRequestDisplayRequired);
    CloseHandle(handle);
}

HResult take_screenshot(This, const void*, void*) {
    G6SHIM_INFO("xgameruntime: screenshot not taken");
    return kNotSupported;
}

const std::array<const void*, 5> kHdrVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&missing<3>), slot(&try_enable_hdr)};

const std::array<const void*, 6> kShellVtable = {
    slot(&missing<0>),  slot(&add_ref),       slot(&add_ref),
    slot(&launch_uri),  slot(&acquire_deferral), slot(&close_deferral)};

const std::array<const void*, 6> kCaptureVtable = {
    slot(&missing<0>), slot(&add_ref),     slot(&add_ref),   slot(&missing<3>),
    slot(&missing<4>), slot(&take_screenshot)};

}  // namespace

const Object kHdrObject{kHdrVtable.data(), "hdr"};
const Object kShellObject{kShellVtable.data(), "shell"};
const Object kCaptureObject{kCaptureVtable.data(), "capture"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {
        {kHdrProvider, kHdrIid, &kHdrObject},
        {kShell, kShell, &kShellObject},
        {kCaptureProvider, kCaptureIid2026, &kCaptureObject},
    };
    return entries;
}

}  // namespace g6shim::xgameruntime::display

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::display_interfaces() {
    return display::interfaces();
}
