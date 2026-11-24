// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indirect.hpp"

#include <g6shim/log.hpp>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace g6shim::d3d12_xs::indirect {

namespace {

constexpr const char* kSource = R"hlsl(
#define RS "RootConstants(num32BitConstants=4, b0), UAV(u0), UAV(u1)"
cbuffer Record : register(b0) { uint kind; uint stride; uint records; uint unused; };
RWByteAddressBuffer args : register(u0);
RWByteAddressBuffer budget : register(u1);
static const uint BUDGET = 1u << 27;
static const uint GROUPS = 1u << 20;
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= records) return;
    uint at = id.x * stride;
    uint cost;
    if (kind <= 1) {
        uint count = min(args.Load(at), 1u << 24);
        uint instances = min(args.Load(at + 4), (1u << 26) / max(count, 1u));
        cost = count * instances;
        args.Store2(at, uint2(count, instances));
    } else if (kind == 2) {
        uint3 g = min(args.Load3(at), uint3(65535, 65535, 65535));
        if (g.z != 0 && g.x * g.y > GROUPS / g.z) {
            g = uint3(0, 0, 0);
        }
        cost = g.x * g.y * g.z * 64;
        args.Store3(at, g);
    } else {
        uint3 g = args.Load3(at);
        g = uint3(min(g.x, 65535u), min(g.y, 1u), min(g.z, 1u));
        cost = g.x * g.y * g.z * 128;
        args.Store3(at, g);
    }
    uint before;
    budget.InterlockedAdd(0, cost, before);
    if (before >= BUDGET || before + cost > BUDGET) {
        args.Store(at, 0);
    }
}
)hlsl";

struct Clamp {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

Clamp* make_clamp() {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT compiled = ::D3DCompile(kSource, std::strlen(kSource), "clamp", nullptr, nullptr, "main",
                                          "cs_5_1", 0, 0, &code, &errors);
    if (FAILED(compiled)) {
        if (errors != nullptr)
            G6SHIM_WARN("d3d12_xs: argument clamp: %s",
                        static_cast<const char*>(errors->GetBufferPointer()));
        else
            G6SHIM_WARN("d3d12_xs: argument clamp did not compile (0x%08lX)",
                        static_cast<unsigned long>(compiled));
        return nullptr;
    }
    if (code == nullptr) return nullptr;
    auto made = std::make_unique<Clamp>();
    if (FAILED(gpu->device()->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(),
                                                  IID_PPV_ARGS(&made->root))))
        return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = made->root.Get();
    description.CS.pShaderBytecode = code->GetBufferPointer();
    description.CS.BytecodeLength = code->GetBufferSize();
    if (FAILED(gpu->device()->CreateComputePipelineState(&description, IID_PPV_ARGS(&made->pso)))) return nullptr;
    return made.release();
}

Clamp* clamp() {
    static std::once_flag once;
    static Clamp* made = nullptr;
    std::call_once(once, [] {
        made = make_clamp();
        if (made == nullptr) G6SHIM_WARN("d3d12_xs: no argument clamp; indirect execution is dropped");
    });
    return made;
}

D3D12_RESOURCE_BARRIER uav_barrier() {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = nullptr;
    return barrier;
}

}  // namespace

bool clamp_available() { return clamp() != nullptr; }

bool bound(host::HostList& list, ID3D12Resource* buffer, std::uint64_t gpu, std::uint32_t kind,
           std::uint32_t stride, std::uint32_t records) {
    Clamp* available = clamp();
    if (available == nullptr) return false;
    const auto budget = list.budget();
    if (!budget) return false;
    if (gpu % 4 != 0 || stride % 4 != 0) return false;
    ID3D12GraphicsCommandList* commands = list.list();
    const std::array<std::uint32_t, 4> constants{kind, stride, records, 0};
    const D3D12_RESOURCE_BARRIER write_barrier = uav_barrier();
    commands->ResourceBarrier(1, &write_barrier);
    commands->SetComputeRootSignature(available->root.Get());
    commands->SetPipelineState(available->pso.Get());
    commands->SetComputeRoot32BitConstants(0, 4, constants.data(), 0);
    commands->SetComputeRootUnorderedAccessView(1, gpu);
    commands->SetComputeRootUnorderedAccessView(2, *budget);
    commands->Dispatch((records + 63) / 64, 1, 1);
    const std::array<D3D12_RESOURCE_BARRIER, 2> barriers{
        uav_barrier(),
        host::transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
    };
    commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    return true;
}

void done(host::HostList& list, ID3D12Resource* buffer) {
    const D3D12_RESOURCE_BARRIER barrier = host::transition(
        buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list.list()->ResourceBarrier(1, &barrier);
}

}  // namespace g6shim::d3d12_xs::indirect
