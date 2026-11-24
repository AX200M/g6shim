// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dxgi.hpp"

#include "console.hpp"
#include "device.hpp"

#include <g6shim/log.hpp>

#include <cstring>

namespace g6shim::d3d12_xs::dxgi {

namespace {

constexpr wchar_t kAdapterName[] = L"g6shim console GPU";

struct Mode {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t refresh[2];
    std::uint32_t format;
    std::uint32_t scanlines;
    std::uint32_t scaling;
    std::uint32_t flags;
};
static_assert(sizeof(Mode) == 32);

HRESULT query(std::size_t kind, const Iid* iid, std::span<const Iid> known, Object* found, Object** out) {
    return answer(kind, iid, is_iid(iid, known) ? found : nullptr, out);
}

HRESULT device_query(Object* object, const Iid* iid, Object** out) {
    if (is_iid(iid, std::span<const Iid>(&::g6shim::d3d12_xs::device::kIid, 1)))
        return answer(kDxgiDevice, iid, ::g6shim::d3d12_xs::device::device(), out);
    return query(kDxgiDevice, iid, kDeviceIids, object, out);
}

HRESULT device_parent(Object*, const Iid* iid, Object** out) {
    return query(kDxgiDevice, iid, kAdapterIids, adapter_object(), out);
}

HRESULT device_adapter(Object*, Object** out) {
    return answer(kDxgiDevice, &kAdapterIids[0], adapter_object(), out);
}

HRESULT adapter_query(Object* object, const Iid* iid, Object** out) {
    return query(kAdapter, iid, kAdapterIids, object, out);
}

HRESULT adapter_parent(Object*, const Iid* iid, Object** out) {
    return query(kAdapter, iid, kFactoryIids, factory_object(), out);
}

HRESULT adapter_outputs(Object*, std::uint32_t index, Object** out) {
    if (index != 0) return kNotFound;
    return answer(kAdapter, &kOutputIids[0], output_object(), out);
}

HRESULT adapter_description(Object*, AdapterDescription* out) {
    if (out == nullptr) return E_INVALIDARG;
    AdapterDescription description{};
    for (std::size_t i = 0; i < sizeof kAdapterName / sizeof kAdapterName[0] - 1; ++i)
        description.name[i] = static_cast<std::uint16_t>(kAdapterName[i]);
    const auto [gpu_optimal, standard] = title_memory(current_console());
    description.vendor = kAmd;
    description.device = 0;
    description.subsystem = 0;
    description.revision = 0;
    description.dedicated_video_memory = gpu_optimal;
    description.dedicated_system_memory = 0;
    description.shared_system_memory = standard;
    description.luid[0] = 1;
    description.luid[1] = 0;
    std::memcpy(out, &description, sizeof description);
    return S_OK;
}

HRESULT factory_query(Object* object, const Iid* iid, Object** out) {
    return query(kFactory, iid, kFactoryIids, object, out);
}

HRESULT factory_adapters(Object*, std::uint32_t index, Object** out) {
    if (index != 0) return kNotFound;
    return answer(kFactory, &kAdapterIids[0], adapter_object(), out);
}

HRESULT output_query(Object* object, const Iid* iid, Object** out) {
    return query(kOutput, iid, kOutputIids, object, out);
}

HRESULT output_parent(Object*, const Iid* iid, Object** out) {
    return query(kOutput, iid, kAdapterIids, adapter_object(), out);
}

HRESULT modes(Object*, std::uint32_t, std::uint32_t, std::uint32_t* count, Mode* out) {
    if (count == nullptr) return E_INVALIDARG;
    if (out == nullptr) {
        *count = 1;
        return S_OK;
    }
    if (*count < 1) return kMoreData;
    *count = 1;
    Mode mode{};
    mode.width = 3840;
    mode.height = 2160;
    mode.refresh[0] = 60;
    mode.refresh[1] = 1;
    mode.scanlines = 1;
    std::memcpy(out, &mode, sizeof mode);
    return S_OK;
}

const std::array<const void*, 32>& device_functions() {
    static const std::array<const void*, 32> table = [] {
        std::array<const void*, 32> f = missing_table<kDxgiDevice, 32>();
        f[0] = reinterpret_cast<const void*>(&device_query);
        f[6] = reinterpret_cast<const void*>(&device_parent);
        f[7] = reinterpret_cast<const void*>(&device_adapter);
        return f;
    }();
    return table;
}

const std::array<const void*, 24>& adapter_functions() {
    static const std::array<const void*, 24> table = [] {
        std::array<const void*, 24> f = missing_table<kAdapter, 24>();
        f[0] = reinterpret_cast<const void*>(&adapter_query);
        f[6] = reinterpret_cast<const void*>(&adapter_parent);
        f[7] = reinterpret_cast<const void*>(&adapter_outputs);
        f[8] = reinterpret_cast<const void*>(&adapter_description);
        return f;
    }();
    return table;
}

const std::array<const void*, 32>& factory_functions() {
    static const std::array<const void*, 32> table = [] {
        std::array<const void*, 32> f = missing_table<kFactory, 32>();
        f[0] = reinterpret_cast<const void*>(&factory_query);
        f[7] = reinterpret_cast<const void*>(&factory_adapters);
        f[12] = reinterpret_cast<const void*>(&factory_adapters);
        return f;
    }();
    return table;
}

const std::array<const void*, 32>& output_functions() {
    static const std::array<const void*, 32> table = [] {
        std::array<const void*, 32> f = missing_table<kOutput, 32>();
        f[0] = reinterpret_cast<const void*>(&output_query);
        f[6] = reinterpret_cast<const void*>(&output_parent);
        f[21] = reinterpret_cast<const void*>(&modes);
        return f;
    }();
    return table;
}

}  // namespace

Object* adapter_object() {
    static Object singleton(adapter_functions().data());
    return &singleton;
}

Object* factory_object() {
    static Object singleton(factory_functions().data());
    return &singleton;
}

Object* output_object() {
    static Object singleton(output_functions().data());
    return &singleton;
}

Object* device() {
    static Object singleton(device_functions().data());
    return &singleton;
}

}  // namespace g6shim::d3d12_xs::dxgi
