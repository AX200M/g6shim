// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "format.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>

namespace g6shim::d3d12_xs::bvh {

struct AddressSpace {
    D3D12_GPU_VIRTUAL_ADDRESS host_base;
    std::uint64_t size;
};

struct PrebuildSizes {
    std::uint64_t result_max;
    std::uint64_t scratch;
    std::uint64_t update_scratch;
};

HRESULT prebuild_sizes(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, PrebuildSizes* out);

class Builder {
public:
    static std::unique_ptr<Builder> create(ID3D12Device* device);
    ~Builder();
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    HRESULT build(ID3D12GraphicsCommandList* list, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& desc,
                  const AddressSpace& space);
    HRESULT copy(ID3D12GraphicsCommandList* list, std::uint64_t dest, std::uint64_t source, std::uint64_t dest_size,
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode, const AddressSpace& space);
    HRESULT postbuild(ID3D12GraphicsCommandList* list,
                      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& desc, std::uint64_t source,
                      const AddressSpace& space);

private:
    Builder(ID3D12RootSignature* signature, ID3D12PipelineState* pipeline);
    ID3D12RootSignature* signature_;
    ID3D12PipelineState* pipeline_;
};

}  // namespace g6shim::d3d12_xs::bvh
