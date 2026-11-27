// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <d3d12.h>

#include <cstdint>

namespace g6shim::d3d12_xs::private_api {

struct UpscalerBridge {
    ID3D12Device* (*device)();
    ID3D12GraphicsCommandList* (*command_list)(void* title_list);
    ID3D12Resource* (*resource)(void* title_list, void* title_resource, std::uint32_t wanted, std::uint32_t* state);
    void (*forget_bindings)(void* title_list);
};

using UpscalerBridgeFn = const UpscalerBridge*(WINAPI*)();

inline const UpscalerBridge* resolve_upscaler_bridge() {
    HMODULE module = ::GetModuleHandleW(L"d3d12_xs.dll");
    if (module == nullptr) return nullptr;
    const auto find = reinterpret_cast<UpscalerBridgeFn>(::GetProcAddress(module, "g6shim_d3d12_xs_upscaler_bridge"));
    return find != nullptr ? find() : nullptr;
}

}  // namespace g6shim::d3d12_xs::private_api

extern "C" const g6shim::d3d12_xs::private_api::UpscalerBridge* WINAPI g6shim_d3d12_xs_upscaler_bridge();
