// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture.hpp"

#include "g_depth_copy_ps.h"
#include "g_depth_copy_stencil_bit_ps.h"
#include "g_depth_copy_stencil_ps.h"
#include "g_depth_copy_vs.h"
#include "removal.hpp"
#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace g6shim::d3d12_xs::texture {

namespace {
constexpr std::uint32_t kDepthWrite = 0x10;
constexpr auto kPreviewFeature = static_cast<D3D12_FEATURE>(72);
constexpr std::uint32_t kStencilBits = 8;

struct PreviewCaps {
    UINT group_cs;
    UINT group_as;
    UINT group_ms;
    BOOL uav;
    BOOL interleaved;
};

struct Caps {
    bool uav = false;
    bool interleaved = false;
    bool stencil_ref = false;
};

struct Note {
    Object* resource = nullptr;
    bool stencil = false;

    bool operator==(const Note& other) const { return resource == other.resource && stencil == other.stencil; }
};

struct Copier {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depths;
    UINT view_step = 0;
    std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> depth;
    std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> stencil_ref;
    std::unordered_map<std::uint32_t, std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kStencilBits>> stencil_bits;
    bool ready = false;
};

std::mutex g_notes_mutex;
std::unordered_map<host::HostList*, std::vector<Note>> g_notes;
std::mutex g_copier_mutex;
Copier g_copier;

Caps caps() {
    static const Caps value = [] {
        Caps out;
        host::Host* gpu = host::host();
        if (gpu == nullptr) return out;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(gpu->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options)))
            out.stencil_ref = options.PSSpecifiedStencilRefSupported != FALSE;
        PreviewCaps preview{};
        if (SUCCEEDED(gpu->device()->CheckFeatureSupport(kPreviewFeature, &preview, sizeof preview)) &&
            preview.uav != FALSE) {
            out.uav = true;
            out.interleaved = preview.interleaved != FALSE;
            G6SHIM_INFO("d3d12_xs: host depth unordered views %s", out.interleaved ? "interleaved" : "on");
        }
        return out;
    }();
    return value;
}

std::optional<std::pair<float, std::uint32_t>> clear_of(const void* clear) {
    if (clear == nullptr) return std::nullopt;
    std::array<std::uint32_t, 5> words{};
    std::memcpy(words.data(), clear, sizeof words);
    float depth = 0;
    std::memcpy(&depth, &words[1], sizeof depth);
    return std::make_pair(depth, words[2] & 0xFFu);
}

struct TwinChoice {
    std::uint32_t colour = 0;
    std::uint32_t depth = 0;
    bool stencil = false;
    bool integer_colour = false;
};

std::optional<TwinChoice> twin_choice(std::uint32_t format) {
    switch (format) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return TwinChoice{DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM, false, false};
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return TwinChoice{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT, false, false};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return TwinChoice{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, true, false};
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return TwinChoice{DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, true, false};
    default: return std::nullopt;
    }
}

std::uint32_t direct_format(std::uint32_t resource, bool stencil, bool interleaved) {
    switch (resource) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        if (interleaved) return DXGI_FORMAT_R32G32_UINT;
        return stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return resource;
    }
}

bool stencil_view(const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, bool has_stencil) {
    if (desc == nullptr) return false;
    UINT plane = 0;
    if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) plane = desc->Texture2D.PlaneSlice;
    if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) plane = desc->Texture2DArray.PlaneSlice;
    const auto format = static_cast<std::uint32_t>(desc->Format);
    return plane == 1 || format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ||
           format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || (has_stencil && format == DXGI_FORMAT_R8_UINT);
}

D3D12_UNORDERED_ACCESS_VIEW_DESC decoded(const std::uint32_t* words) {
    std::array<std::uint32_t, 10> values{};
    if (words != nullptr) std::memcpy(values.data(), words, sizeof values);
    values[0] = resource::pc_format(values[0]);
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    std::memcpy(&desc, values.data(), sizeof desc);
    return desc;
}

void barrier_plane(ID3D12GraphicsCommandList* commands, later::DepthUavPlane& plane, D3D12_RESOURCE_STATES wanted) {
    if (plane.texture == nullptr) return;
    const auto before = static_cast<D3D12_RESOURCE_STATES>(
        plane.state.exchange(static_cast<std::uint32_t>(wanted), std::memory_order_relaxed));
    if (before == wanted) return;
    const D3D12_RESOURCE_BARRIER barrier = host::transition(plane.texture.Get(), before, wanted);
    commands->ResourceBarrier(1, &barrier);
}

void force_depth(host::HostList& list, Object* object, std::uint32_t wanted) {
    const std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (!copy || copy->texture == nullptr) return;
    const std::uint32_t legal = host::legal_state(copy->texture.Get(), wanted);
    const std::uint32_t before = copy->state.exchange(legal, std::memory_order_relaxed);
    if (before != legal) {
        const D3D12_RESOURCE_BARRIER barrier =
            host::transition(copy->texture.Get(), static_cast<D3D12_RESOURCE_STATES>(before),
                             static_cast<D3D12_RESOURCE_STATES>(legal));
        list.list()->ResourceBarrier(1, &barrier);
    }
    list.note_state(object, legal);
}

Microsoft::WRL::ComPtr<ID3D12Resource> make_twin(ID3D12Device* device, const D3D12_RESOURCE_DESC& depth,
                                                 std::uint32_t format, const char* label) {
    D3D12_RESOURCE_DESC desc = depth;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    const HRESULT made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                          IID_PPV_ARGS(&texture));
    if (FAILED(made)) {
        G6SHIM_WARN("d3d12_xs: no depth unordered twin (format %u, 0x%08lX)", format,
                    static_cast<unsigned long>(made));
        return nullptr;
    }
    host::name(texture.Get(), label);
    return texture;
}

void clear_plane(ID3D12Resource* texture, bool integer, float depth, std::uint32_t stencil) {
    host::Host* gpu = host::host();
    if (gpu == nullptr || texture == nullptr) return;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(gpu->device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)))) return;
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = texture->GetDesc().Format;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = heap->GetGPUDescriptorHandleForHeapStart();
    gpu->device()->CreateUnorderedAccessView(texture, nullptr, &view, cpu);
    const std::unique_ptr<host::HostList> commands = gpu->new_list();
    if (!commands) return;
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    commands->list()->SetDescriptorHeaps(1, heaps);
    if (integer) {
        const std::array<UINT, 4> values{stencil, 0, 0, 0};
        commands->list()->ClearUnorderedAccessViewUint(gpu_handle, cpu, texture, values.data(), 0, nullptr);
    } else {
        const std::array<float, 4> values{depth, 0, 0, 0};
        commands->list()->ClearUnorderedAccessViewFloat(gpu_handle, cpu, texture, values.data(), 0, nullptr);
    }
    commands->close();
    gpu->execute({commands.get()});
}

D3D12_SHADER_BYTECODE code(const unsigned char* bytes, std::size_t size) {
    return D3D12_SHADER_BYTECODE{bytes, size};
}

bool ensure_copier(ID3D12Device* device) {
    if (g_copier.ready) return g_copier.root != nullptr;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 2;
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    const HRESULT serial = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(serial)) {
        G6SHIM_WARN("d3d12_xs: depth copy root signature refused (0x%08lX)", static_cast<unsigned long>(serial));
        g_copier.ready = true;
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_copier.root)))) {
        G6SHIM_WARN("d3d12_xs: depth copy root signature refused");
        g_copier.ready = true;
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = 2;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC depths{};
    depths.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    depths.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&g_copier.views))) ||
        FAILED(device->CreateDescriptorHeap(&depths, IID_PPV_ARGS(&g_copier.depths)))) {
        G6SHIM_WARN("d3d12_xs: no depth copy descriptors");
        g_copier.ready = true;
        return false;
    }
    g_copier.view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_copier.ready = true;
    return true;
}

D3D12_DEPTH_STENCIL_DESC depth_state(bool write_depth, bool write_stencil, std::uint32_t stencil_mask) {
    D3D12_DEPTH_STENCIL_DESC state{};
    state.DepthEnable = write_depth ? TRUE : FALSE;
    state.DepthWriteMask = write_depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    state.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.StencilEnable = write_stencil ? TRUE : FALSE;
    state.StencilReadMask = 0xFF;
    state.StencilWriteMask = static_cast<UINT8>(stencil_mask);
    const D3D12_DEPTH_STENCILOP_DESC op{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE,
                                        D3D12_COMPARISON_FUNC_ALWAYS};
    state.FrontFace = op;
    state.BackFace = op;
    return state;
}

ID3D12PipelineState* pipeline(ID3D12Device* device, std::uint32_t dsv, const unsigned char* pixel, std::size_t pixel_bytes,
                              bool write_depth, bool write_stencil, std::uint32_t stencil_mask,
                              std::unordered_map<std::uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>>& cache) {
    const auto known = cache.find(dsv | (stencil_mask << 16));
    if (known != cache.end()) return known->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = g_copier.root.Get();
    desc.VS = code(g_depth_copy_vs, sizeof g_depth_copy_vs);
    desc.PS = code(pixel, pixel_bytes);
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState = depth_state(write_depth, write_stencil, stencil_mask);
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
    desc.DSVFormat = static_cast<DXGI_FORMAT>(dsv);
    desc.SampleDesc.Count = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> made;
    const HRESULT result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&made));
    if (FAILED(result)) {
        G6SHIM_WARN("d3d12_xs: depth copy pipeline refused (format %u, 0x%08lX)", dsv,
                    static_cast<unsigned long>(result));
        cache.emplace(dsv | (stencil_mask << 16), nullptr);
        return nullptr;
    }
    const auto [entry, inserted] = cache.emplace(dsv | (stencil_mask << 16), std::move(made));
    (void)inserted;
    return entry->second.Get();
}

void bind_srv(ID3D12Device* device, ID3D12Resource* texture, std::uint32_t format, UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_copier.views->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g_copier.view_step;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture, &desc, cpu);
}

void draw_copy(ID3D12GraphicsCommandList* commands, ID3D12PipelineState* state, ID3D12Resource* depth,
               std::uint32_t dsv_format, std::uint32_t width, std::uint32_t height, std::uint32_t constant,
               std::uint32_t stencil_ref) {
    host::Host* gpu = host::host();
    if (gpu == nullptr || state == nullptr) return;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = static_cast<DXGI_FORMAT>(dsv_format);
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE depth_view = g_copier.depths->GetCPUDescriptorHandleForHeapStart();
    gpu->device()->CreateDepthStencilView(depth, &dsv, depth_view);
    ID3D12DescriptorHeap* heaps[] = {g_copier.views.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(g_copier.root.Get());
    commands->SetPipelineState(state);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(std::max(width, 1u));
    viewport.Height = static_cast<float>(std::max(height, 1u));
    viewport.MaxDepth = 1;
    commands->RSSetViewports(1, &viewport);
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(std::max(width, 1u)), static_cast<LONG>(std::max(height, 1u))};
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(0, nullptr, FALSE, &depth_view);
    commands->OMSetStencilRef(stencil_ref);
    commands->SetGraphicsRootDescriptorTable(0, g_copier.views->GetGPUDescriptorHandleForHeapStart());
    commands->SetGraphicsRoot32BitConstant(1, constant, 0);
    removal::mark(commands, "depth copy");
    commands->DrawInstanced(3, 1, 0, 0);
}

bool copy_plane(host::HostList& list, Object* object, bool stencil) {
    host::Host* gpu = host::host();
    const std::shared_ptr<later::HostTexture> copy = resource::host_copy(object);
    if (gpu == nullptr || !copy || !copy->depth_uav || copy->texture == nullptr) return false;
    later::DepthUavPlane& plane = stencil ? copy->depth_uav->stencil : copy->depth_uav->depth;
    if (plane.texture == nullptr) return false;
    const auto choice = twin_choice(static_cast<std::uint32_t>(copy->texture->GetDesc().Format));
    if (!choice) return false;
    std::lock_guard<std::mutex> lock(g_copier_mutex);
    if (!ensure_copier(gpu->device())) return false;
    ID3D12GraphicsCommandList* commands = list.list();
    const D3D12_RESOURCE_DESC desc = copy->texture->GetDesc();
    if (stencil) {
        bind_srv(gpu->device(), nullptr, DXGI_FORMAT_R32_FLOAT, 0);
        bind_srv(gpu->device(), plane.texture.Get(), plane.format, 1);
    } else {
        bind_srv(gpu->device(), plane.texture.Get(), plane.format, 0);
        bind_srv(gpu->device(), nullptr, DXGI_FORMAT_R8_UINT, 1);
    }
    if (!stencil) {
        ID3D12PipelineState* state = pipeline(gpu->device(), choice->depth, g_depth_copy_ps, sizeof g_depth_copy_ps,
                                              true, false, 0, g_copier.depth);
        if (state == nullptr) return false;
        barrier_plane(commands, plane, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        force_depth(list, object, kDepthWrite);
        draw_copy(commands, state, copy->texture.Get(), choice->depth, static_cast<std::uint32_t>(desc.Width),
                  desc.Height, 0, 0);
        return true;
    }
    barrier_plane(commands, plane, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    force_depth(list, object, kDepthWrite);
    if (caps().stencil_ref) {
        ID3D12PipelineState* state =
            pipeline(gpu->device(), choice->depth, g_depth_copy_stencil_ps, sizeof g_depth_copy_stencil_ps, false,
                     true, 0xFF, g_copier.stencil_ref);
        if (state != nullptr) {
            draw_copy(commands, state, copy->texture.Get(), choice->depth, static_cast<std::uint32_t>(desc.Width),
                      desc.Height, 0, 0);
            return true;
        }
    }
    bool drew = false;
    for (std::uint32_t bit = 0; bit < kStencilBits; ++bit) {
        const auto known = g_copier.stencil_bits.find(choice->depth);
        if (known == g_copier.stencil_bits.end())
            g_copier.stencil_bits.emplace(choice->depth, std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8>{});
        auto& slot = g_copier.stencil_bits[choice->depth][bit];
        if (slot == nullptr) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = g_copier.root.Get();
            pso.VS = code(g_depth_copy_vs, sizeof g_depth_copy_vs);
            pso.PS = code(g_depth_copy_stencil_bit_ps, sizeof g_depth_copy_stencil_bit_ps);
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.RasterizerState.DepthClipEnable = TRUE;
            pso.DepthStencilState = depth_state(false, true, 1u << bit);
            pso.SampleMask = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.DSVFormat = static_cast<DXGI_FORMAT>(choice->depth);
            pso.SampleDesc.Count = 1;
            if (FAILED(gpu->device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&slot)))) {
                G6SHIM_WARN("d3d12_xs: stencil bit copy refused (bit %u)", bit);
                slot.Reset();
                continue;
            }
        }
        draw_copy(commands, slot.Get(), copy->texture.Get(), choice->depth, static_cast<std::uint32_t>(desc.Width),
                  desc.Height, bit, 1u << bit);
        drew = true;
    }
    return drew;
}

}  // namespace

bool depth_unordered_direct(std::uint32_t format, std::uint32_t samples) {
    if (samples > 1) return false;
    switch (format) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return caps().uav;
    default: return false;
    }
}

void attach_depth_uav(later::HostTexture& host, const resource::ResourceDesc& host_desc, bool direct,
                      const void* clear) {
    if (host.texture == nullptr) return;
    if (direct) {
        static std::once_flag once;
        std::call_once(once, [] { G6SHIM_INFO("d3d12_xs: depth unordered access uses host depth views"); });
        host.depth_uav = std::make_shared<later::DepthUav>();
        host.depth_uav->direct = true;
        return;
    }
    if (host_desc.dimension != 3 || host_desc.sample_count > 1) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a depth unordered texture is not a single-sample 2D image");
        });
        return;
    }
    const auto choice = twin_choice(host_desc.format);
    if (!choice) {
        static std::once_flag once;
        std::call_once(once, [] { G6SHIM_WARN("d3d12_xs: a depth unordered texture has no colour twin"); });
        return;
    }
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    auto twins = std::make_shared<later::DepthUav>();
    const auto cleared = clear_of(clear);
    const float depth = cleared ? cleared->first : 0;
    const std::uint32_t stencil = cleared ? cleared->second : 0;
    twins->depth.texture = make_twin(gpu->device(), host.texture->GetDesc(), choice->colour, "depth unordered twin");
    twins->depth.format = choice->colour;
    twins->depth.clear_float = depth;
    if (twins->depth.texture) clear_plane(twins->depth.texture.Get(), false, depth, 0);
    if (choice->stencil) {
        twins->stencil.texture = make_twin(gpu->device(), host.texture->GetDesc(), DXGI_FORMAT_R8_UINT,
                                           "stencil unordered twin");
        twins->stencil.format = DXGI_FORMAT_R8_UINT;
        twins->stencil.clear_uint = stencil;
        if (twins->stencil.texture) clear_plane(twins->stencil.texture.Get(), true, 0, stencil);
    }
    if (twins->depth.texture == nullptr && twins->stencil.texture == nullptr) return;
    static std::once_flag once;
    std::call_once(once, [] { G6SHIM_INFO("d3d12_xs: depth unordered access uses colour twins"); });
    host.depth_uav = std::move(twins);
}

std::optional<DepthUavBinding> depth_uav_binding(Object* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc) {
    const std::shared_ptr<later::HostTexture> copy = resource::host_copy(resource);
    if (!copy || !copy->depth_uav || copy->texture == nullptr) return std::nullopt;
    const bool stencil = stencil_view(desc, copy->depth_uav->stencil.texture != nullptr);
    DepthUavBinding binding;
    binding.plane = 0;
    if (copy->depth_uav->direct) {
        binding.resource = copy->texture.Get();
        binding.format = direct_format(static_cast<std::uint32_t>(copy->texture->GetDesc().Format), stencil,
                                       caps().interleaved);
        binding.plane = stencil && !caps().interleaved ? 1u : 0u;
        binding.twin = false;
        return binding;
    }
    later::DepthUavPlane& plane = stencil ? copy->depth_uav->stencil : copy->depth_uav->depth;
    if (plane.texture == nullptr) return std::nullopt;
    binding.resource = plane.texture.Get();
    binding.format = plane.format;
    binding.twin = true;
    return binding;
}

void note_depth_uav(host::HostList& list, Object* resource, const std::uint32_t* words) {
    const std::shared_ptr<later::HostTexture> copy = resource::host_copy(resource);
    if (!copy || !copy->depth_uav || copy->depth_uav->direct) return;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = decoded(words);
    const bool stencil = words != nullptr && stencil_view(&desc, copy->depth_uav->stencil.texture != nullptr);
    later::DepthUavPlane& plane = stencil ? copy->depth_uav->stencil : copy->depth_uav->depth;
    if (plane.texture == nullptr) return;
    barrier_plane(list.list(), plane, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const Note note{resource, stencil};
    std::lock_guard<std::mutex> lock(g_notes_mutex);
    auto& notes = g_notes[&list];
    if (std::find(notes.begin(), notes.end(), note) == notes.end()) notes.push_back(note);
}

bool copy_depth_uavs(host::HostList& list) {
    std::vector<Note> notes;
    {
        std::lock_guard<std::mutex> lock(g_notes_mutex);
        const auto found = g_notes.find(&list);
        if (found == g_notes.end() || found->second.empty()) return false;
        notes = std::move(found->second);
        g_notes.erase(found);
    }
    bool copied = false;
    for (const Note& note : notes) copied = copy_plane(list, note.resource, note.stencil) || copied;
    return copied;
}

void drop_depth_uav_notes(host::HostList* list) {
    std::lock_guard<std::mutex> lock(g_notes_mutex);
    g_notes.erase(list);
}

}  // namespace g6shim::d3d12_xs::texture
