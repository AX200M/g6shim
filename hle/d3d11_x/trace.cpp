// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "trace.hpp"

#include <windows.h>
#include <d3d11.h>

namespace g6shim::d3d {

namespace {

constexpr std::uintptr_t kLowestUserAddress = 0x10000;

struct Kind {
    GUID iid;
    const char* name;
};

const char* object_kind_unsafe(std::uintptr_t value) {
    if (value < kLowestUserAddress) return "-";
    void* vtable = *reinterpret_cast<void**>(value);
    if (reinterpret_cast<std::uintptr_t>(vtable) < kLowestUserAddress) return "data";
    void* first = *static_cast<void**>(vtable);
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(first), &module))
        return "data";
    static const Kind kinds[] = {{__uuidof(ID3D11ShaderResourceView), "SRV"},
                                 {__uuidof(ID3D11Buffer), "buffer"},
                                 {__uuidof(ID3D11Texture2D), "texture2d"},
                                 {__uuidof(ID3D11SamplerState), "sampler"},
                                 {__uuidof(ID3D11PixelShader), "pixel shader"},
                                 {__uuidof(ID3D11VertexShader), "vertex shader"},
                                 {__uuidof(ID3D11InputLayout), "input layout"},
                                 {__uuidof(ID3D11RenderTargetView), "RTV"},
                                 {__uuidof(ID3D11BlendState), "blend state"},
                                 {__uuidof(ID3D11DeviceContext), "context"}};
    auto* unknown = reinterpret_cast<IUnknown*>(value);
    for (const auto& k : kinds) {
        void* out = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(k.iid, &out)) && out) {
            static_cast<IUnknown*>(out)->Release();
            return k.name;
        }
    }
    return "COM?";
}

}  // namespace

const char* object_kind(std::uintptr_t value) {
    __try {
        return object_kind_unsafe(value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "bad";
    }
}

std::uintptr_t peek(std::uintptr_t value) {
    __try {
        return value >= kLowestUserAddress ? *reinterpret_cast<std::uintptr_t*>(value) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace g6shim::d3d
