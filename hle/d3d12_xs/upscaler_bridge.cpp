// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "private.hpp"

#include "host.hpp"
#include "list.hpp"
#include "object.hpp"
#include "texture.hpp"

namespace g6shim::d3d12_xs {

namespace {

ID3D12Device* bridge_device() {
    host::Host* gpu = host::host();
    return gpu != nullptr ? gpu->device() : nullptr;
}

ID3D12GraphicsCommandList* bridge_command_list(void* title_list) {
    host::HostList* list = list::host_list(static_cast<Object*>(title_list));
    return list != nullptr ? list->list() : nullptr;
}

ID3D12Resource* bridge_resource(void* title_list, void* title_resource, std::uint32_t wanted, std::uint32_t* state) {
    host::HostList* list = list::host_list(static_cast<Object*>(title_list));
    auto* resource = static_cast<Object*>(title_resource);
    ID3D12Resource* texture = texture::host_texture(resource);
    if (list == nullptr || texture == nullptr) return nullptr;
    host::ensure_state(*list, resource, wanted);
    if (state != nullptr) *state = host::legal_state(texture, wanted);
    return texture;
}

void bridge_forget_bindings(void* title_list) { list::forget_host_bindings(static_cast<Object*>(title_list)); }

constexpr private_api::UpscalerBridge kBridge{&bridge_device, &bridge_command_list, &bridge_resource,
                                              &bridge_forget_bindings};

}  // namespace

}  // namespace g6shim::d3d12_xs

extern "C" const g6shim::d3d12_xs::private_api::UpscalerBridge* WINAPI g6shim_d3d12_xs_upscaler_bridge() {
    return &g6shim::d3d12_xs::kBridge;
}
