// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xuser.hpp"

#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <array>
#include <string>
#include <string_view>

namespace g6shim::xgameruntime::xuser {

namespace {

using Handle = const void*;
using This = Object*;

constexpr char kGamertag[] = "Player";

const std::uint8_t kUserStorage = 0;
const std::uint8_t kAddIdentity = 0;
const std::uint8_t kResolveIdentity = 0;
const std::uint8_t kTokenIdentity = 0;

Handle user() { return &kUserStorage; }

HResult found(bool present, Handle* out) {
    if (!present) return kUserNotFound;
    return put(out, user());
}

HResult add_provider(std::uint32_t operation, const xasync::ProviderData* data) {
    switch (operation) {
        case xasync::kOpBegin:
            xasync::XAsyncComplete(data->block, kOk, sizeof(Handle));
            break;
        case xasync::kOpGetResult:
            return put(static_cast<Handle*>(data->buffer), user());
        default:
            break;
    }
    return kOk;
}

HResult duplicate(This, Handle, Handle* out) { return put(out, user()); }
void close(This, Handle) {}
std::int32_t compare(This, Handle first, Handle second) { return first != second ? 1 : 0; }

HResult add_async(This, std::uint32_t options, xasync::AsyncBlock* block) {
    G6SHIM_INFO("xgameruntime: XUserAddAsync options=0x%x: the offline local user", options);
    return xasync::XAsyncBegin(block, nullptr, &kAddIdentity, nullptr, &add_provider);
}

HResult add_result(This, xasync::AsyncBlock* block, Handle* out) {
    return xasync::XAsyncGetResult(block, &kAddIdentity, sizeof(Handle), out, nullptr);
}

HResult get_local_id(This, Handle, std::uint64_t* out) { return put(out, kLocalId); }

HResult find_by_local_id(This, std::uint64_t id, Handle* out) { return found(id == kLocalId, out); }

HResult get_id(This, Handle, std::uint64_t* out) { return put(out, kXuid); }

HResult find_by_id(This, std::uint64_t id, Handle* out) { return found(id == kXuid, out); }

HResult get_is_guest(This, Handle, bool* out) { return put(out, false); }

HResult get_state(This, Handle, std::uint32_t* out) { return put(out, std::uint32_t{0}); }

HResult get_gamertag(This, Handle, std::uint32_t component, std::size_t size, std::uint8_t* buffer,
                     std::size_t* used) {
    const std::string_view value = component == kModernSuffixComponent ? std::string_view{} : kGamertag;
    return put_string(value, size, buffer, used);
}

HResult check_privilege(This, Handle, std::uint32_t, std::uint32_t, bool* has, std::uint32_t* reason) {
    if (reason != nullptr) *reason = 0;
    return put(has, true);
}

HResult get_age_group(This, Handle, std::uint32_t* out) { return put(out, kAdultAgeGroup); }

HResult resolve_privilege_async(This, Handle, std::uint32_t, std::uint32_t, xasync::AsyncBlock* block) {
    return xasync::finished(block, &kResolveIdentity, kOk);
}

HResult resolve_privilege_result(This, xasync::AsyncBlock* block) {
    return xasync::XAsyncGetResult(block, &kResolveIdentity, 0, nullptr, nullptr);
}

HResult token_async(This, Handle, std::uint32_t, const char*, const char* url, std::size_t, const void*,
                    std::size_t, const void*, xasync::AsyncBlock* block) {
    G6SHIM_INFO("xgameruntime: token and signature for %s: offline", text(url).value_or("").c_str());
    return xasync::finished(block, &kTokenIdentity, kFailedToGetToken);
}

HResult token_size(This, xasync::AsyncBlock* block, std::size_t* out) {
    return xasync::XAsyncGetResultSize(block, out);
}

HResult token_result(This, xasync::AsyncBlock* block, std::size_t size, void* buffer, const void**,
                     std::size_t* used) {
    return xasync::XAsyncGetResult(block, &kTokenIdentity, size, buffer, used);
}

HResult register_change(This, const void*, void*, const void*, std::uint64_t* out) { return put(out, token()); }

bool unregister_change(This, std::uint64_t, bool) { return true; }

HResult find_for_device(This, const void*, Handle*) { return kUserNotFound; }

const std::array<const void*, 35> kUserVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&duplicate),
    slot(&close),
    slot(&compare),
    slot(&missing<6>),
    slot(&add_async),
    slot(&add_result),
    slot(&get_local_id),
    slot(&find_by_local_id),
    slot(&get_id),
    slot(&find_by_id),
    slot(&get_is_guest),
    slot(&get_state),
    slot(&get_gamertag),
    slot(&missing<16>),
    slot(&missing<17>),
    slot(&missing<18>),
    slot(&get_age_group),
    slot(&check_privilege),
    slot(&resolve_privilege_async),
    slot(&resolve_privilege_result),
    slot(&token_async),
    slot(&token_size),
    slot(&token_result),
    slot(&missing<26>),
    slot(&missing<27>),
    slot(&missing<28>),
    slot(&missing<29>),
    slot(&missing<30>),
    slot(&missing<31>),
    slot(&missing<32>),
    slot(&register_change),
    slot(&unregister_change),
};

const std::array<const void*, 4> kGamertagVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&get_gamertag),
};

const std::array<const void*, 6> kDeviceVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&find_for_device),
    slot(&register_change),
    slot(&unregister_change),
};

}  // namespace

const Object kObject{kUserVtable.data(), "user"};
const Object kGamertagObject{kGamertagVtable.data(), "user gamertag"};
const Object kDeviceObject{kDeviceVtable.data(), "user devices"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {
        {kProvider, kIid, &kObject},
        {kProvider, kProvider, &kObject},
        {kProvider, kGamertagIid, &kGamertagObject},
        {kDeviceProvider, kDeviceIid, &kDeviceObject},
    };
    return entries;
}

}  // namespace g6shim::xgameruntime::xuser

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xuser_interfaces() {
    return xuser::interfaces();
}
