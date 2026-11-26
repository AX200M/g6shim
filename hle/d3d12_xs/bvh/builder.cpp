// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "builder.hpp"

#include "g_bvh_build_dxil.h"
#include "../removal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace g6shim::d3d12_xs::bvh {

namespace {

constexpr std::uint32_t kModeInit = 0;
constexpr std::uint32_t kModeTriangles = 1;
constexpr std::uint32_t kModeInstances = 2;
constexpr std::uint32_t kModeMorton = 3;
constexpr std::uint32_t kModeKarras = 4;
constexpr std::uint32_t kModeNodes = 5;
constexpr std::uint32_t kModeHeader = 6;
constexpr std::uint32_t kModeRadixCount = 7;
constexpr std::uint32_t kModeRadixScan = 8;
constexpr std::uint32_t kModeRadixScatter = 9;
constexpr std::uint32_t kModeSmallSort = 10;
constexpr std::uint32_t kModeCopy = 12;
constexpr std::uint32_t kModePostbuild = 13;
constexpr std::uint32_t kModeKept = 14;
constexpr std::uint32_t kModeKeptScan = 15;
constexpr std::uint32_t kModeKeptIndex = 16;
constexpr std::uint32_t kModePropagate = 17;
constexpr std::uint32_t kModeClear = 18;

constexpr std::uint32_t kFlagTlas = 1;
constexpr std::uint32_t kFlagBox16 = 2;
constexpr std::uint32_t kFlagPair = 4;
constexpr std::uint32_t kPushDwords = 26;
constexpr std::uint32_t kGroupThreads = 64;
constexpr std::uint32_t kParentInitFloor = 16;
constexpr std::uint32_t kIndexNone = 0;
constexpr std::uint32_t kIndexR16 = 1;
constexpr std::uint32_t kIndexR32 = 2;
constexpr std::uint32_t kXyVertex = 1;
constexpr std::uint32_t kGeometryFlagBits = 0xffu;
constexpr std::uint32_t kOfflinePostbuild = 0x10u;
constexpr std::uint32_t kMaxDescriptors = 1u << 20;
constexpr std::uint32_t kVertexAlign = 4;
constexpr std::uint32_t kInstanceAlign = 16;
constexpr std::uint32_t kTransformBytes = 48;
constexpr std::uint32_t kPostbuildBytes = 8;
constexpr std::uint64_t kWindowCap = 1ull << 32;
constexpr std::uint32_t kRootConstants = 0;
constexpr std::uint32_t kRootDst = 1;
constexpr std::uint32_t kRootScratch = 2;
constexpr std::uint32_t kRootSrc0 = 3;
constexpr std::uint32_t kRootSrc1 = 4;
constexpr std::uint32_t kRootSrc2 = 5;
constexpr std::uint32_t kRootWindow = 6;
constexpr std::uint32_t kRootUavCount = 6;

struct Addr {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
};

struct Push {
    Addr dst{};
    Addr scratch{};
    Addr src0{};
    Addr src1{};
    Addr src2{};
    Addr bias{};
    std::uint32_t mode = 0;
    std::uint32_t count = 0;
    std::uint32_t first = 0;
    std::uint32_t arg0 = 0;
    std::uint32_t arg1 = 0;
    std::uint32_t arg2 = 0;
    std::uint32_t geometry = 0;
    std::uint32_t elements = 0;
    std::uint32_t sort_size = 0;
    std::uint32_t elements_off = 0;
    std::uint32_t nodes_off = 0;
    std::uint32_t parents_off = 0;
    std::uint32_t total_size = 0;
    std::uint32_t flags = 0;
};

static_assert(sizeof(Push) == kPushDwords * sizeof(std::uint32_t));
static_assert(offsetof(Push, flags) == (kPushDwords - 1) * sizeof(std::uint32_t));

struct Plan {
    bool top = false;
    bool box16 = false;
    bool pair = false;
    bool update = false;
    std::uint32_t elements = 0;
};

Addr split(std::uint64_t value) {
    return {static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)};
}

bool fits(const AddressSpace& space, std::uint64_t address, std::uint64_t bytes) {
    if (space.host_base == 0) return false;
    if (bytes == 0) return true;
    if (address > space.size || bytes > space.size - address) return false;
    return space.host_base <= std::numeric_limits<std::uint64_t>::max() - address;
}

std::uint32_t window_limit(const AddressSpace& space) {
    const std::uint64_t cap = space.size < kWindowCap ? space.size : kWindowCap;
    if (cap < kHeaderSize) return 0;
    return static_cast<std::uint32_t>(cap - kHeaderSize);
}

bool aligned(std::uint64_t address, std::uint32_t alignment) { return (address & (alignment - 1u)) == 0; }

const D3D12_RAYTRACING_GEOMETRY_DESC* geometry_at(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                                                  UINT index) {
    if (inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY) {
        return inputs.pGeometryDescs == nullptr ? nullptr : inputs.pGeometryDescs + index;
    }
    if (inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) {
        return inputs.ppGeometryDescs == nullptr ? nullptr : inputs.ppGeometryDescs[index];
    }
    return nullptr;
}

HRESULT triangle_count(const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, std::uint32_t* out) {
    std::uint32_t count = 0;
    if (triangles.IndexFormat == DXGI_FORMAT_UNKNOWN) {
        if (triangles.IndexCount != 0 || (triangles.VertexCount % 3u) != 0) return E_INVALIDARG;
        count = triangles.VertexCount / 3u;
    } else if (triangles.IndexFormat == DXGI_FORMAT_R16_UINT || triangles.IndexFormat == DXGI_FORMAT_R32_UINT) {
        if ((triangles.IndexCount % 3u) != 0) return E_INVALIDARG;
        count = triangles.IndexCount / 3u;
    } else {
        return E_NOTIMPL;
    }
    if (count > 0 && triangles.VertexFormat != DXGI_FORMAT_R32G32B32_FLOAT &&
        triangles.VertexFormat != DXGI_FORMAT_R32G32_FLOAT) {
        return E_NOTIMPL;
    }
    *out = count;
    return S_OK;
}

HRESULT classify(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, Plan* plan) {
    if (inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL &&
        inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        return E_INVALIDARG;
    }
    const bool top = inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    const auto flags = static_cast<std::uint32_t>(inputs.Flags);
    const bool update = (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
    const bool allow = (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) != 0;
    if (update && !allow) return E_INVALIDARG;
    if (inputs.NumDescs > kMaxDescriptors) return E_INVALIDARG;
    if (top && inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) return E_NOTIMPL;
    if (!top && inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY &&
        inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) {
        return E_INVALIDARG;
    }
    std::uint32_t count = 0;
    if (top) {
        count = inputs.NumDescs;
    } else if (inputs.NumDescs > 0) {
        for (UINT index = 0; index < inputs.NumDescs; ++index) {
            const D3D12_RAYTRACING_GEOMETRY_DESC* geometry = geometry_at(inputs, index);
            if (geometry == nullptr) return E_INVALIDARG;
            if (geometry->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) return E_NOTIMPL;
            if (geometry->Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) return E_INVALIDARG;
            std::uint32_t triangles = 0;
            const HRESULT result = triangle_count(geometry->Triangles, &triangles);
            if (FAILED(result)) return result;
            if (count > std::numeric_limits<std::uint32_t>::max() - triangles) return E_INVALIDARG;
            count += triangles;
        }
    }
    plan->top = top;
    plan->update = update;
    plan->box16 = !top && (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY) != 0;
    plan->pair = !top && !allow && !update;
    plan->elements = count;
    return S_OK;
}

HRESULT triangle_range(const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles, std::uint32_t count,
                       const AddressSpace& space) {
    if (count == 0) return S_OK;
    const bool xy = triangles.VertexFormat == DXGI_FORMAT_R32G32_FLOAT;
    const std::uint32_t component = xy ? 8u : 12u;
    if (triangles.VertexCount > 0) {
        if (triangles.VertexBuffer.StartAddress == 0 || !aligned(triangles.VertexBuffer.StartAddress, kVertexAlign))
            return E_INVALIDARG;
        if (triangles.VertexBuffer.StrideInBytes < component || (triangles.VertexBuffer.StrideInBytes % kVertexAlign) != 0)
            return E_INVALIDARG;
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(triangles.VertexCount) * triangles.VertexBuffer.StrideInBytes;
        if (triangles.VertexBuffer.StrideInBytes != 0 &&
            bytes / triangles.VertexBuffer.StrideInBytes != triangles.VertexCount) {
            return E_INVALIDARG;
        }
        if (!fits(space, triangles.VertexBuffer.StartAddress, bytes)) return E_INVALIDARG;
    }
    if (triangles.IndexFormat != DXGI_FORMAT_UNKNOWN) {
        if (triangles.IndexBuffer == 0) return E_INVALIDARG;
        const std::uint32_t index_bytes = triangles.IndexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u;
        const std::uint32_t index_align = index_bytes;
        if (!aligned(triangles.IndexBuffer, index_align)) return E_INVALIDARG;
        const std::uint64_t bytes = static_cast<std::uint64_t>(count) * 3u * index_bytes;
        if (!fits(space, triangles.IndexBuffer, bytes)) return E_INVALIDARG;
    }
    if (triangles.Transform3x4 != 0) {
        if (!aligned(triangles.Transform3x4, kVertexAlign) || !fits(space, triangles.Transform3x4, kTransformBytes))
            return E_INVALIDARG;
    }
    return S_OK;
}

Push base_push(const Layout& layout, std::uint64_t dest, std::uint64_t scratch, const AddressSpace& space) {
    Push push{};
    push.dst = split(dest);
    push.scratch = split(scratch);
    push.bias.lo = window_limit(space);
    push.elements = layout.elements;
    push.sort_size = layout.sort_size;
    push.elements_off = layout.elements_offset;
    push.nodes_off = layout.nodes_offset;
    push.parents_off = layout.parents_offset;
    push.total_size = layout.size;
    push.flags = (layout.top_level ? kFlagTlas : 0u) | (layout.box16 ? kFlagBox16 : 0u);
    push.arg0 = layout.nodes;
    return push;
}

class Recorder {
public:
    Recorder(ID3D12GraphicsCommandList* list, ID3D12RootSignature* signature, ID3D12PipelineState* pipeline,
             const AddressSpace& space)
        : list_(list), signature_(signature), pipeline_(pipeline), space_(space) {}

    void set_dest(std::uint64_t dest, std::uint64_t scratch) {
        dest_ = dest;
        scratch_ = scratch;
    }

    void barrier() {
        D3D12_RESOURCE_BARRIER wait{};
        wait.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list_->ResourceBarrier(1, &wait);
    }

    void dispatch(const Push& push, std::uint32_t threads, std::uint64_t src0, std::uint64_t src1, std::uint64_t src2,
                  bool align_src1, bool wait) {
        if (!bound_) {
            list_->SetComputeRootSignature(signature_);
            list_->SetPipelineState(pipeline_);
            bound_ = true;
        }
        list_->SetComputeRoot32BitConstants(kRootConstants, kPushDwords, &push, 0);
        list_->SetComputeRootUnorderedAccessView(kRootDst, space_.host_base + dest_);
        list_->SetComputeRootUnorderedAccessView(kRootScratch, space_.host_base + scratch_);
        list_->SetComputeRootUnorderedAccessView(kRootSrc0, view(src0, false));
        list_->SetComputeRootUnorderedAccessView(kRootSrc1, view(src1, align_src1));
        list_->SetComputeRootUnorderedAccessView(kRootSrc2, view(src2, false));
        list_->SetComputeRootUnorderedAccessView(kRootWindow, space_.host_base);
        removal::mark(list_, "acceleration");
        list_->Dispatch((threads + kGroupThreads - 1u) / kGroupThreads, 1, 1);
        if (wait) barrier();
    }

private:
    D3D12_GPU_VIRTUAL_ADDRESS view(std::uint64_t address, bool align_down) const {
        if (address == 0) return space_.host_base + dest_;
        const std::uint64_t console = align_down ? (address & ~std::uint64_t{3}) : address;
        return space_.host_base + console;
    }

    ID3D12GraphicsCommandList* list_;
    ID3D12RootSignature* signature_;
    ID3D12PipelineState* pipeline_;
    AddressSpace space_;
    std::uint64_t dest_ = 0;
    std::uint64_t scratch_ = 0;
    bool bound_ = false;
};

void begin(Recorder& recorder, const Layout& layout, const Push& push) {
    Push init = push;
    init.mode = kModeInit;
    init.count = layout.parent_entries > kParentInitFloor ? layout.parent_entries : kParentInitFloor;
    recorder.dispatch(init, init.count, 0, 0, 0, false, true);
}

void refit_begin(Recorder& recorder, const Layout& layout, const Push& push, std::uint64_t source) {
    const std::uint64_t dest = (static_cast<std::uint64_t>(push.dst.hi) << 32) | push.dst.lo;
    if (source == dest) return;
    Push copy = push;
    copy.mode = kModeCopy;
    copy.count = layout.size / 4u;
    copy.src0 = split(source);
    recorder.dispatch(copy, copy.count, source, 0, 0, false, true);
}

void propagate(Recorder& recorder, const Layout& layout, const Push& push, bool update) {
    if (layout.elements == 0) return;
    Push step = push;
    step.mode = kModePropagate;
    step.count = layout.elements;
    step.arg2 = update ? 1u : 0u;
    recorder.dispatch(step, step.count, 0, 0, 0, false, true);
}

void refit_finish(Recorder& recorder, const Layout& layout, const Push& push) {
    Push clear = push;
    clear.mode = kModeClear;
    clear.count = layout.nodes;
    recorder.dispatch(clear, clear.count, 0, 0, 0, false, true);
    propagate(recorder, layout, push, true);
    Push header = push;
    header.mode = kModeHeader;
    header.count = 1;
    header.arg2 = 1;
    recorder.dispatch(header, 1, 0, 0, 0, false, false);
}

void finish(Recorder& recorder, const Layout& layout, const Push& push) {
    Push morton = push;
    morton.mode = kModeMorton;
    morton.count = layout.sort_size;
    recorder.dispatch(morton, morton.count, 0, 0, 0, false, true);
    if (layout.sort_size > 1 && layout.sort_size <= kSmallSortMax) {
        Push sort = push;
        sort.mode = kModeSmallSort;
        recorder.dispatch(sort, kGroupThreads, 0, 0, 0, false, true);
    } else if (layout.sort_size > 1) {
        for (std::uint32_t pass = 0; pass < kRadixPasses; ++pass) {
            Push sort = push;
            sort.first = pass;
            sort.mode = kModeRadixCount;
            recorder.dispatch(sort, kGroupThreads * radix_tiles(layout), 0, 0, 0, false, true);
            sort.mode = kModeRadixScan;
            recorder.dispatch(sort, kGroupThreads, 0, 0, 0, false, true);
            sort.mode = kModeRadixScatter;
            recorder.dispatch(sort, kGroupThreads * radix_tiles(layout), 0, 0, 0, false, true);
        }
    }
    const std::uint32_t inner = inner_nodes(layout);
    if (inner != 0) {
        Push tree = push;
        tree.mode = kModeKarras;
        tree.count = inner;
        recorder.dispatch(tree, tree.count, 0, 0, 0, false, true);
        tree.mode = kModeKept;
        recorder.dispatch(tree, kGroupThreads * kept_tiles(layout), 0, 0, 0, false, true);
        tree.mode = kModeKeptScan;
        recorder.dispatch(tree, kGroupThreads, 0, 0, 0, false, true);
        tree.mode = kModeKeptIndex;
        recorder.dispatch(tree, kGroupThreads * kept_tiles(layout), 0, 0, 0, false, true);
    }
    Push nodes = push;
    nodes.mode = kModeNodes;
    nodes.count = inner != 0 ? inner : 1u;
    recorder.dispatch(nodes, nodes.count, 0, 0, 0, false, true);
    propagate(recorder, layout, push, false);
    Push header = push;
    header.mode = kModeHeader;
    header.count = 1;
    recorder.dispatch(header, 1, 0, 0, 0, false, false);
}

}  // namespace

Builder::Builder(ID3D12RootSignature* signature, ID3D12PipelineState* pipeline)
    : signature_(signature), pipeline_(pipeline) {}

Builder::~Builder() {
    if (pipeline_ != nullptr) pipeline_->Release();
    if (signature_ != nullptr) signature_->Release();
}

std::unique_ptr<Builder> Builder::create(ID3D12Device* device) {
    if (device == nullptr) return nullptr;
    D3D12_ROOT_PARAMETER parameters[1 + kRootUavCount]{};
    parameters[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kRootConstants].Constants.Num32BitValues = kPushDwords;
    parameters[kRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::uint32_t slot = 0; slot < kRootUavCount; ++slot) {
        D3D12_ROOT_PARAMETER& parameter = parameters[1 + slot];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = slot;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1 + kRootUavCount;
    desc.pParameters = parameters;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (error != nullptr) error->Release();
    if (FAILED(result)) return nullptr;
    ID3D12RootSignature* signature = nullptr;
    result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature));
    blob->Release();
    if (FAILED(result)) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = signature;
    pipeline_desc.CS.pShaderBytecode = g_bvh_build_dxil;
    pipeline_desc.CS.BytecodeLength = sizeof(g_bvh_build_dxil);
    ID3D12PipelineState* pipeline = nullptr;
    result = device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        signature->Release();
        return nullptr;
    }
    auto* builder = new (std::nothrow) Builder(signature, pipeline);
    if (builder == nullptr) {
        pipeline->Release();
        signature->Release();
        return nullptr;
    }
    return std::unique_ptr<Builder>(builder);
}

HRESULT prebuild_sizes(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, PrebuildSizes* out) {
    if (out == nullptr) return E_INVALIDARG;
    Plan plan;
    const HRESULT result = classify(inputs, &plan);
    if (FAILED(result)) return result;
    Layout layout{};
    if (!compute_layout(plan.elements, plan.top, plan.box16, &layout)) return E_INVALIDARG;
    out->result_max = layout.size;
    out->scratch = layout.scratch;
    out->update_scratch = layout.scratch;
    return S_OK;
}

HRESULT Builder::build(ID3D12GraphicsCommandList* list, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& desc,
                       const AddressSpace& space) {
    if (list == nullptr) return E_INVALIDARG;
    Plan plan;
    const HRESULT classified = classify(desc.Inputs, &plan);
    if (FAILED(classified)) return classified;
    if (!aligned(desc.DestAccelerationStructureData, kStructureAlignment)) return E_INVALIDARG;
    Layout layout{};
    if (!compute_layout(plan.elements, plan.top, plan.box16, &layout)) return E_INVALIDARG;
    if (!fits(space, desc.DestAccelerationStructureData, layout.size) ||
        !fits(space, desc.ScratchAccelerationStructureData, layout.scratch)) {
        return E_INVALIDARG;
    }
    if (plan.update) {
        if (desc.SourceAccelerationStructureData == 0) return E_INVALIDARG;
        if (desc.SourceAccelerationStructureData != desc.DestAccelerationStructureData &&
            !fits(space, desc.SourceAccelerationStructureData, layout.size)) {
            return E_INVALIDARG;
        }
    }
    if (!plan.top) {
        for (UINT index = 0; index < desc.Inputs.NumDescs; ++index) {
            const D3D12_RAYTRACING_GEOMETRY_DESC* geometry = geometry_at(desc.Inputs, index);
            std::uint32_t count = 0;
            const HRESULT counted = triangle_count(geometry->Triangles, &count);
            if (FAILED(counted)) return counted;
            const HRESULT ranged = triangle_range(geometry->Triangles, count, space);
            if (FAILED(ranged)) return ranged;
        }
    } else if (plan.elements > 0) {
        if (desc.Inputs.InstanceDescs == 0 || !aligned(desc.Inputs.InstanceDescs, kInstanceAlign)) return E_INVALIDARG;
        const std::uint64_t bytes = static_cast<std::uint64_t>(plan.elements) * kInstanceSize;
        if (!fits(space, desc.Inputs.InstanceDescs, bytes)) return E_INVALIDARG;
    }
    Recorder recorder(list, signature_, pipeline_, space);
    recorder.set_dest(desc.DestAccelerationStructureData, desc.ScratchAccelerationStructureData);
    Push push = base_push(layout, desc.DestAccelerationStructureData, desc.ScratchAccelerationStructureData, space);
    if (plan.pair) push.flags |= kFlagPair;
    if (plan.update) refit_begin(recorder, layout, push, desc.SourceAccelerationStructureData);
    else begin(recorder, layout, push);
    if (!plan.top) {
        for (UINT index = 0; index < desc.Inputs.NumDescs; ++index) {
            const D3D12_RAYTRACING_GEOMETRY_DESC* geometry = geometry_at(desc.Inputs, index);
            std::uint32_t count = 0;
            triangle_count(geometry->Triangles, &count);
            if (count == 0) continue;
            const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles = geometry->Triangles;
            Push step = push;
            step.mode = kModeTriangles;
            step.count = count;
            step.first = push.first;
            step.src0 = split(triangles.VertexBuffer.StartAddress);
            step.src1 = split(triangles.IndexBuffer);
            step.src2 = split(triangles.Transform3x4);
            step.arg0 = static_cast<std::uint32_t>(triangles.VertexBuffer.StrideInBytes);
            step.arg1 = triangles.VertexFormat == DXGI_FORMAT_R32G32_FLOAT ? kXyVertex : 0u;
            step.arg2 = triangles.IndexFormat == DXGI_FORMAT_R32_UINT
                            ? kIndexR32
                            : (triangles.IndexFormat == DXGI_FORMAT_R16_UINT ? kIndexR16 : kIndexNone);
            step.geometry = (static_cast<std::uint32_t>(index) << 8) |
                            (static_cast<std::uint32_t>(geometry->Flags) & kGeometryFlagBits);
            step.bias.hi = triangles.VertexCount;
            recorder.dispatch(step, count, triangles.VertexBuffer.StartAddress, triangles.IndexBuffer,
                              triangles.Transform3x4, step.arg2 == kIndexR16, true);
            push.first += count;
        }
    } else if (layout.elements != 0) {
        Push step = push;
        step.mode = kModeInstances;
        step.count = layout.elements;
        step.src0 = split(desc.Inputs.InstanceDescs);
        recorder.dispatch(step, step.count, desc.Inputs.InstanceDescs, 0, 0, false, true);
    }
    if (plan.update) refit_finish(recorder, layout, push);
    else finish(recorder, layout, push);
    return S_OK;
}

HRESULT Builder::copy(ID3D12GraphicsCommandList* list, std::uint64_t dest, std::uint64_t source, std::uint64_t dest_size,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode, const AddressSpace& space) {
    if (list == nullptr) return E_INVALIDARG;
    if (mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE &&
        mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) {
        return E_NOTIMPL;
    }
    if (source == 0 || dest == 0 || dest_size < kHeaderSize) return E_INVALIDARG;
    if (!fits(space, source, kHeaderSize) || !fits(space, dest, kHeaderSize)) return E_INVALIDARG;
    if (source == dest) return S_OK;
    const std::uint64_t dest_room = space.size - dest;
    const std::uint64_t source_room = space.size - source;
    const std::uint64_t capped = dest_size < dest_room ? dest_size : dest_room;
    const std::uint64_t bytes = capped < source_room ? capped : source_room;
    if (bytes < kHeaderSize) return E_INVALIDARG;
    const std::uint32_t words = static_cast<std::uint32_t>((bytes > 0xffffffffull ? 0xffffffffull : bytes) / 4u);
    Recorder recorder(list, signature_, pipeline_, space);
    recorder.set_dest(dest, dest);
    Push push{};
    push.mode = kModeCopy;
    push.src0 = split(source);
    push.dst = split(dest);
    push.count = words;
    recorder.dispatch(push, words, source, 0, 0, false, false);
    return S_OK;
}

HRESULT Builder::postbuild(ID3D12GraphicsCommandList* list,
                           const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& desc,
                           std::uint64_t source, const AddressSpace& space) {
    if (list == nullptr) return E_INVALIDARG;
    const auto type = static_cast<std::uint32_t>(desc.InfoType);
    if (type == kOfflinePostbuild) return E_INVALIDARG;
    if (type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE &&
        type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE) {
        return E_NOTIMPL;
    }
    if (source == 0 || desc.DestBuffer == 0) return E_INVALIDARG;
    if (!fits(space, source, kHeaderSize) || !fits(space, desc.DestBuffer, kPostbuildBytes)) return E_INVALIDARG;
    Recorder recorder(list, signature_, pipeline_, space);
    recorder.barrier();
    recorder.set_dest(desc.DestBuffer, desc.DestBuffer);
    Push push{};
    push.mode = kModePostbuild;
    push.src0 = split(source);
    push.dst = split(desc.DestBuffer);
    push.count = 1;
    recorder.dispatch(push, 1, source, 0, 0, false, false);
    return S_OK;
}

}  // namespace g6shim::d3d12_xs::bvh
