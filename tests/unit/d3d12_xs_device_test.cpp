// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/device.hpp"
#include "d3d12_xs/object.hpp"

#include <utility>

using namespace g6shim::d3d12_xs;

namespace {

std::pair<HRESULT, Object*> create_as(const Iid& iid) {
    const std::uint32_t version = device::kCheckedVersion;
    Object* created = nullptr;
    const HRESULT hr = device::create(nullptr, &version, &iid, &created);
    return {hr, created};
}

}  // namespace

TEST(d3d12_xs_creation_hands_out_the_device) {
    const auto [hr, created] = create_as(device::kIid);
    CHECK(hr == S_OK);
    CHECK(created == device::device());
    CHECK((reinterpret_cast<std::uint8_t*>(created)[0x18]) != 0);
    CHECK(create_as(Iid{}).first == E_NOINTERFACE);
    CHECK(slot<std::uint32_t (*)(Object*)>(created, 0x38)(created) == 1);
    const auto missing = slot<HRESULT (*)()>(created, 0xf0);
    CHECK(missing() == E_NOTIMPL);
}

TEST(d3d12_xs_newer_device_levels_are_the_same_device) {
    const auto [hr, created] = create_as(device::kLevelIids[8]);
    CHECK(hr == S_OK);
    CHECK(created == device::device());
    Object* same = nullptr;
    CHECK(slot<HRESULT (*)(Object*, const Iid*, Object**)>(created, 0)(created, &device::kLevelIids[4], &same) ==
          S_OK);
    CHECK(same == created);
    const auto missing = slot<HRESULT (*)()>(created, 0x3a0);
    CHECK(missing() == E_NOTIMPL);
}

TEST(d3d12_xs_virtual_memory_configuration_reports_user_space) {
    device::VirtualMemory config{};
    config.page_table_pages = 7;
    CHECK(device::configure_virtual_memory(&config) == S_OK);
    CHECK(config.lowest > 0 && config.lowest < config.highest);
    auto* heap = new std::uint8_t(0);
    const auto address = reinterpret_cast<std::uint64_t>(heap);
    CHECK(address >= config.lowest && address <= config.highest);
    delete heap;
    CHECK(device::configure_virtual_memory(nullptr) == E_INVALIDARG);
}

TEST(d3d12_xs_feature_levels_pick_the_highest_supported) {
    using Check = HRESULT (*)(Object*, std::uint32_t, void*, std::uint32_t);
    Object* created = device::device();
    const auto check = slot<Check>(created, 0x68);
    const std::array<std::uint32_t, 5> levels{0xB000, 0xB100, 0xC000, 0xC100, 0xD000};
    device::FeatureLevels query{};
    query.count = static_cast<std::uint32_t>(levels.size());
    query.requested = levels.data();
    CHECK(check(created, 2, &query, 0x18) == S_OK);
    CHECK(query.highest == 0xC100);
    query.highest = 0;
    CHECK(check(created, 2, &query, 0x10) == E_INVALIDARG);
    device::FeatureLevels too_new{};
    too_new.count = 1;
    too_new.requested = &levels[4];
    CHECK(check(created, 2, &too_new, 0x18) == static_cast<HRESULT>(0x887A0004u));
}
