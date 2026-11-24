// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/device.hpp"
#include "d3d12_xs/dxgi.hpp"
#include "d3d12_xs/fence.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/object.hpp"

#include <d3d12.h>
#include <wrl/client.h>

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

TEST(d3d12_xs_feature_levels_pick_the_highest_supported) {    using Check = HRESULT (*)(Object*, std::uint32_t, void*, std::uint32_t);
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

TEST(d3d12_xs_the_device_is_found_at_every_level_through_others) {
    Object* fence_object = nullptr;
    CHECK(fence::create(nullptr, 0, 0, &fence::kIids[0], &fence_object) == S_OK);
    using Get = HRESULT (*)(Object*, const Iid*, Object**);
    const auto get_device = slot<Get>(fence_object, 0x38);
    Object* dxgi = dxgi::device();
    const auto query = slot<Get>(dxgi, 0);
    auto check_one = [&](const Iid& iid) {
        Object* found = nullptr;
        CHECK(get_device(fence_object, &iid, &found) == S_OK);
        CHECK(found == device::device());
        found = nullptr;
        CHECK(query(dxgi, &iid, &found) == S_OK);
        CHECK(found == device::device());
    };
    check_one(device::kIid);
    for (const Iid& level : device::kLevelIids) check_one(level);
    Object* found = device::device();
    CHECK(get_device(fence_object, &fence::kIids[0], &found) == E_NOINTERFACE);
    CHECK(found == nullptr);
    CHECK(slot<std::uint32_t (*)(Object*)>(fence_object, 0x10)(fence_object) == 0);
}

TEST(d3d12_xs_host_children_hand_out_newer_device_levels) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_object;
    CHECK(SUCCEEDED(gpu->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_object))));
    Microsoft::WRL::ComPtr<ID3D12Device5> newer_device;
    CHECK(SUCCEEDED(fence_object->GetDevice(IID_PPV_ARGS(&newer_device))));
    CHECK(newer_device != nullptr);
}
