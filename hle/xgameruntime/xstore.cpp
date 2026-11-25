// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xstore.hpp"

#include "title.hpp"
#include "xasync.hpp"
#include "xpackage.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>

namespace g6shim::xgameruntime::xstore {

namespace {

using This = Object*;
using Handle = const void*;

struct Marker {
    std::uint8_t value = 0;
};

Marker kContext;
Marker kQuery;
Marker kValid;
Marker kInvalid;

std::uint8_t kFailed = 0;
std::uint8_t kQueryCall = 0;
std::uint8_t kLicenseCall = 0;
std::uint8_t kGameLicense = 0;

Handle handle(const Marker& marker) { return &marker; }

HResult offline(xasync::AsyncBlock* block, const char* what) {
    G6SHIM_INFO("xgameruntime: store call offline: %s", what);
    return xasync::finished(block, &kFailed, kNetworkError);
}

HResult status(This, xasync::AsyncBlock* block) { return xasync::XAsyncGetStatus(block, false); }

HResult create_context(This, const void*, Handle* out) { return put(out, handle(kContext)); }

void close(This, Handle) {}

HResult associated_products_async(This, Handle, std::uint32_t, std::uint32_t, xasync::AsyncBlock* block) {
    return offline(block, "associated products");
}

HResult entitled_products_async(This, Handle, std::uint32_t, std::uint32_t, xasync::AsyncBlock* block) {
    return xasync::finished_with(block, &kQueryCall, kOk, xasync::bytes(handle(kQuery)));
}

HResult query_result(This, xasync::AsyncBlock* block, Handle* out) {
    return xasync::result(block, &kQueryCall, out);
}

HResult enumerate_products(This, Handle, void*, const void*) { return kOk; }

bool has_more_pages(This, Handle) { return false; }

HResult next_page_async(This, Handle, xasync::AsyncBlock* block) {
    return xasync::finished(block, &kQueryCall, kIllegalMethodCall);
}

HResult license_for(bool owned, xasync::AsyncBlock* block) {
    const Handle license = handle(owned ? kValid : kInvalid);
    return xasync::finished_with(block, &kLicenseCall, kOk, xasync::bytes(license));
}

HResult license_for_package_async(This, Handle, const char* package, xasync::AsyncBlock* block) {
    const std::optional<std::string> name = text(package);
    return license_for(name && *name == xpackage::identifier(), block);
}

HResult license_for_durables_async(This, Handle, const char* store_id, xasync::AsyncBlock* block) {
    G6SHIM_TRACE("xgameruntime: durable %s not owned", text(store_id).value_or("").c_str());
    return license_for(false, block);
}

HResult license_result(This, xasync::AsyncBlock* block, Handle* out) {
    return xasync::result(block, &kLicenseCall, out);
}

bool is_license_valid(This, Handle license) { return license == handle(kValid); }

GameLicense game_license() {
    GameLicense value{};
    const std::string id = title().store_id + kSkuSuffix;
    const std::size_t length = std::min(id.size(), kStoreIdBytes - 1);
    std::memcpy(value.sku_store_id.data(), id.data(), length);
    value.is_active = true;
    value.trial_seconds_remaining = 0;
    value.expiration = 0;
    return value;
}

HResult game_license_async(This, Handle, xasync::AsyncBlock* block) {
    return xasync::finished_with(block, &kGameLicense, kOk, xasync::bytes(game_license()));
}

HResult game_license_result(This, xasync::AsyncBlock* block, GameLicense* out) {
    return xasync::result(block, &kGameLicense, out);
}

HResult consumable_balance_async(This, Handle, const char*, xasync::AsyncBlock* block) {
    return offline(block, "consumable balance");
}

HResult report_fulfillment_async(This, Handle, const char*, std::uint32_t, const void*,
                                 xasync::AsyncBlock* block) {
    return offline(block, "consumable fulfillment");
}

HResult user_id_async(This, Handle, const char*, const char*, xasync::AsyncBlock* block) {
    return offline(block, "user store id");
}

HResult size_status(This, xasync::AsyncBlock* block, std::size_t*) {
    return xasync::XAsyncGetStatus(block, false);
}

HResult string_status(This, xasync::AsyncBlock* block, std::size_t, char*) {
    return xasync::XAsyncGetStatus(block, false);
}

HResult count_status(This, xasync::AsyncBlock* block, std::uint32_t*) {
    return xasync::XAsyncGetStatus(block, false);
}

HResult array_status(This, xasync::AsyncBlock* block, std::uint32_t, void*) {
    return xasync::XAsyncGetStatus(block, false);
}

HResult purchase_ui_async(This, Handle, const char*, const char*, const char*, xasync::AsyncBlock* block) {
    return offline(block, "purchase page");
}

HResult product_page_ui_async(This, Handle, const char*, xasync::AsyncBlock* block) {
    return offline(block, "product page");
}

HResult redeem_ui_async(This, Handle, const char*, const char* const*, std::size_t, bool,
                        xasync::AsyncBlock* block) {
    return offline(block, "redeem page");
}

HResult updates_async(This, Handle, xasync::AsyncBlock* block) { return offline(block, "update check"); }

HResult install_async(This, Handle, const char* const*, std::size_t, xasync::AsyncBlock* block) {
    return offline(block, "package download");
}

HResult package_identifier(This, const char* store_id, std::size_t size, std::uint8_t* buffer) {
    const Title value = title();
    const std::optional<std::string> id = text(store_id);
    if (id && !value.store_id.empty() && *id == value.store_id)
        return put_string(xpackage::identifier(), size, buffer, nullptr);
    return xpackage::kNoPackage;
}

HResult register_change(This, Handle, const void*, void*, const void*, std::uint64_t* out) {
    return put(out, token());
}

bool unregister_change(This, Handle, std::uint64_t, bool) { return true; }

const std::array<const void*, 76> kVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&create_context),
    slot(&close),
    slot(&associated_products_async),
    slot(&query_result),
    slot(&missing<7>),
    slot(&missing<8>),
    slot(&entitled_products_async),
    slot(&query_result),
    slot(&missing<11>),
    slot(&missing<12>),
    slot(&missing<13>),
    slot(&missing<14>),
    slot(&enumerate_products),
    slot(&has_more_pages),
    slot(&next_page_async),
    slot(&query_result),
    slot(&close),
    slot(&license_for_package_async),
    slot(&license_result),
    slot(&is_license_valid),
    slot(&close),
    slot(&missing<24>),
    slot(&missing<25>),
    slot(&missing<26>),
    slot(&missing<27>),
    slot(&game_license_async),
    slot(&game_license_result),
    slot(&missing<30>),
    slot(&missing<31>),
    slot(&missing<32>),
    slot(&consumable_balance_async),
    slot(&status),
    slot(&report_fulfillment_async),
    slot(&missing<36>),
    slot(&user_id_async),
    slot(&size_status),
    slot(&string_status),
    slot(&user_id_async),
    slot(&size_status),
    slot(&string_status),
    slot(&missing<43>),
    slot(&missing<44>),
    slot(&missing<45>),
    slot(&missing<46>),
    slot(&missing<47>),
    slot(&missing<48>),
    slot(&purchase_ui_async),
    slot(&missing<50>),
    slot(&missing<51>),
    slot(&missing<52>),
    slot(&redeem_ui_async),
    slot(&missing<54>),
    slot(&updates_async),
    slot(&count_status),
    slot(&array_status),
    slot(&missing<58>),
    slot(&missing<59>),
    slot(&install_async),
    slot(&status),
    slot(&install_async),
    slot(&count_status),
    slot(&array_status),
    slot(&package_identifier),
    slot(&register_change),
    slot(&unregister_change),
    slot(&register_change),
    slot(&unregister_change),
    slot(&missing<70>),
    slot(&license_for_durables_async),
    slot(&license_result),
    slot(&missing<73>),
    slot(&missing<74>),
    slot(&product_page_ui_async),
};

}  // namespace

const Object kObject{kVtable.data(), "store"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kIid2026, &kObject}, {kProvider, kDurablesIid, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::xstore

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xstore_interfaces() {
    return xstore::interfaces();
}
