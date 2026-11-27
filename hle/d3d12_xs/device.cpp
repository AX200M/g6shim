// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.hpp"

#include "descriptor.hpp"
#include "dxgi.hpp"
#include "fence.hpp"
#include "frame.hpp"
#include "later.hpp"
#include "list.hpp"
#include "query.hpp"
#include "queue.hpp"
#include "resource.hpp"
#include "root.hpp"
#include "signature.hpp"

#include <g6shim/log.hpp>

#include <cstring>

namespace g6shim::d3d12_xs::device {

bool is_device(const Iid* iid) {
    if (is_iid(iid, std::span<const Iid>(&kIid, 1))) return true;
    return is_iid(iid, kLevelIids);
}

namespace {

constexpr HRESULT kUnsupported = static_cast<HRESULT>(0x887A0004u);

constexpr std::array<std::uint32_t, 16> kOptions{
    1,
    1,
    2,
    3,
    3,
    1,
    1,
    1,
    3,
    40,
    0,
    0,
    0,
    0,
    1,
    2,
};

std::atomic<bool> g_newer{false};

HRESULT ignore(Object*) { return S_OK; }

std::uint32_t node_count(Object*) { return 1; }

HRESULT query_interface(Object* object, const Iid* iid, Object** out) {
    Object* found = nullptr;
    if (is_device(iid))
        found = object;
    else if (is_iid(iid, dxgi::kDeviceIids))
        found = dxgi::device();
    return answer(kDevice, iid, found, out);
}

HRESULT feature_levels(void* data) {
    auto* query = static_cast<FeatureLevels*>(data);
    if (query->requested == nullptr) return E_INVALIDARG;
    bool found = false;
    std::uint32_t highest = 0;
    for (std::uint32_t i = 0; i < query->count; ++i) {
        const std::uint32_t level = query->requested[i];
        if (level <= kHighestFeatureLevel && (!found || level > highest)) {
            highest = level;
            found = true;
        }
    }
    if (!found) return kUnsupported;
    query->highest = highest;
    return S_OK;
}

HRESULT check_feature_support(Object*, std::uint32_t feature, void* data, std::uint32_t size) {
    constexpr std::uint32_t kOptionsQuery = 0;
    constexpr std::uint32_t kArchitecture = 1;
    constexpr std::uint32_t kFeatureLevelQuery = 2;
    constexpr std::uint32_t kFormatSupport = 3;
    constexpr std::uint32_t kMultisample = 4;
    std::uint32_t expected = 0;
    switch (feature) {
    case kOptionsQuery: expected = sizeof kOptions; break;
    case kArchitecture: expected = 16; break;
    case kFeatureLevelQuery: expected = sizeof(FeatureLevels); break;
    case kFormatSupport: expected = 12; break;
    case kMultisample: expected = 16; break;
    default:
        G6SHIM_WARN("d3d12_xs: device feature query %u (size %u) not implemented", feature, size);
        return E_NOTIMPL;
    }
    if (data == nullptr || size != expected) return E_INVALIDARG;
    switch (feature) {
    case kOptionsQuery:
        std::memcpy(data, kOptions.data(), sizeof kOptions);
        return S_OK;
    case kArchitecture: {
        auto* words = static_cast<std::uint32_t*>(data);
        if (words[0] != 0) return E_INVALIDARG;
        words[1] = 0;
        words[2] = 1;
        words[3] = 1;
        return S_OK;
    }
    case kFormatSupport: {
        auto* words = static_cast<std::uint32_t*>(data);
        words[1] = 0x7FFF'FFFF;
        words[2] = 0xFFFF;
        return S_OK;
    }
    case kMultisample: {
        auto* words = static_cast<std::uint32_t*>(data);
        const std::uint32_t count = words[1];
        words[3] = count == 1 || count == 2 || count == 4 || count == 8 ? 1 : 0;
        return S_OK;
    }
    default:
        return feature_levels(data);
    }
}

std::pair<std::uint64_t, std::uint64_t> user_space() {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress)),
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress))};
}

std::array<const void*, kMaxSlots> make_functions() {
    std::array<const void*, kMaxSlots> f = d3d12_object(missing_table<kDevice, kMaxSlots>());
    f[0] = reinterpret_cast<const void*>(&query_interface);
    f[7] = reinterpret_cast<const void*>(&node_count);
    f[8] = reinterpret_cast<const void*>(&queue::create);
    f[9] = reinterpret_cast<const void*>(&list::create_allocator);
    f[10] = reinterpret_cast<const void*>(&later::create_graphics_pipeline);
    f[11] = reinterpret_cast<const void*>(&later::create_compute_pipeline);
    f[12] = reinterpret_cast<const void*>(&list::create_list);
    f[13] = reinterpret_cast<const void*>(&check_feature_support);
    f[14] = reinterpret_cast<const void*>(&descriptor::create_heap);
    f[15] = reinterpret_cast<const void*>(&descriptor::increment_size);
    f[16] = reinterpret_cast<const void*>(&root::create);
    f[17] = reinterpret_cast<const void*>(&descriptor::create_cbv);
    f[18] = reinterpret_cast<const void*>(&descriptor::create_srv);
    f[19] = reinterpret_cast<const void*>(&descriptor::create_uav);
    f[20] = reinterpret_cast<const void*>(&descriptor::create_rtv);
    f[21] = reinterpret_cast<const void*>(&descriptor::create_dsv);
    f[22] = reinterpret_cast<const void*>(&descriptor::create_sampler);
    f[23] = reinterpret_cast<const void*>(&descriptor::copy);
    f[24] = reinterpret_cast<const void*>(&descriptor::copy_simple);
    f[25] = reinterpret_cast<const void*>(&resource::allocation_info);
    f[26] = reinterpret_cast<const void*>(&resource::custom_heap_properties);
    f[27] = reinterpret_cast<const void*>(&resource::create_committed);
    f[28] = reinterpret_cast<const void*>(&resource::create_heap);
    f[29] = reinterpret_cast<const void*>(&resource::create_placed);
    f[36] = reinterpret_cast<const void*>(&fence::create);
    f[38] = reinterpret_cast<const void*>(&resource::copyable_footprints);
    f[39] = reinterpret_cast<const void*>(&query::create);
    f[41] = reinterpret_cast<const void*>(&signature::create);
    f[48] = reinterpret_cast<const void*>(&queue::create_x);
    f[61] = reinterpret_cast<const void*>(&descriptor::create_sampler_x);
    f[62] = reinterpret_cast<const void*>(&later::create_compute_pipeline_x);
    f[69] = reinterpret_cast<const void*>(&ignore);
    f[70] = reinterpret_cast<const void*>(&descriptor::create_placed_srv);
    f[71] = reinterpret_cast<const void*>(&descriptor::create_placed_uav);
    f[73] = reinterpret_cast<const void*>(&list::create_list_x);
    f[74] = reinterpret_cast<const void*>(&resource::register_page_pool);
    f[75] = reinterpret_cast<const void*>(&resource::unregister_page_pool);
    f[76] = reinterpret_cast<const void*>(&resource::create_committed);
    f[78] = reinterpret_cast<const void*>(&frame::set_interval);
    f[79] = reinterpret_cast<const void*>(&frame::schedule);
    f[80] = reinterpret_cast<const void*>(&frame::wait);
    f[81] = reinterpret_cast<const void*>(&frame::statistics);
    f[86] = reinterpret_cast<const void*>(&later::create_stream_pipeline);
    f[108] = reinterpret_cast<const void*>(&resource::allocation_info2);
    f[127] = reinterpret_cast<const void*>(&resource::create_committed3);
    f[133] = reinterpret_cast<const void*>(&resource::create_at_address);
    return f;
}

Device& device_object() {
    static Device singleton;
    return singleton;
}

}  // namespace

const std::array<const void*, kMaxSlots>& table() {
    static const std::array<const void*, kMaxSlots> table = make_functions();
    return table;
}

Object* device() { return &device_object(); }

bool newer() { return g_newer.load(std::memory_order_relaxed); }

HRESULT create(const void*, const std::uint32_t* parameters, const Iid* iid, Object** out) {
    if (parameters == nullptr || iid == nullptr) return E_INVALIDARG;
    const std::uint32_t version = *parameters;
    G6SHIM_INFO("d3d12_xs: device created for interface version 0x%X", version);
    if (version != kCheckedVersion)
        G6SHIM_WARN("d3d12_xs: device layouts were checked against version 0x%X only", kCheckedVersion);
    if (!is_device(iid)) {
        const std::string text = guid_text(*iid);
        G6SHIM_WARN("d3d12_xs: device created as unknown interface %s", text.c_str());
        return E_NOINTERFACE;
    }
    if (out == nullptr) return S_FALSE;
    if (*iid == kLevelIids[8]) g_newer.store(true, std::memory_order_relaxed);
    return answer(kDevice, iid, device(), out);
}

HRESULT configure_virtual_memory(VirtualMemory* config) {
    if (config == nullptr) return E_INVALIDARG;
    G6SHIM_INFO("d3d12_xs: graphics virtual memory (flags %u, page table %u pages, preserved %u bytes)",
                config->flags, config->page_table_pages, config->preserved_bytes);
    const auto [lowest, highest] = user_space();
    config->lowest = lowest;
    config->highest = highest;
    return S_OK;
}

}  // namespace g6shim::d3d12_xs::device
