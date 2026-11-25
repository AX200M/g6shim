// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_host.hpp"

#include "host.hpp"
#include "pipeline.hpp"
#include "pipeline_library.hpp"
#include "resource.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/emit/shader.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::pipeline_host {

namespace {

constexpr std::uint32_t kRectangles = 19;
constexpr std::uint32_t kQuads = 32;
constexpr std::uint32_t kMaxInputElements = 256;
constexpr std::size_t kMaxSemanticName = 256;

std::string blend_text(const D3D12_BLEND_DESC& blend) {
    std::string out = "blend ";
    for (const D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget) {
        out += std::to_string(target.BlendEnable) + "," + std::to_string(target.LogicOpEnable) + "," +
               std::to_string(static_cast<int>(target.SrcBlend)) + "," +
               std::to_string(static_cast<int>(target.DestBlend)) + "," +
               std::to_string(static_cast<int>(target.BlendOp)) + "," +
               std::to_string(static_cast<int>(target.SrcBlendAlpha)) + "," +
               std::to_string(static_cast<int>(target.DestBlendAlpha)) + "," +
               std::to_string(static_cast<int>(target.BlendOpAlpha)) + "," +
               std::to_string(static_cast<int>(target.LogicOp)) + "," +
               std::to_string(static_cast<int>(target.RenderTargetWriteMask)) + ";";
    }
    return out;
}

std::string op_text(const D3D12_DEPTH_STENCILOP_DESC& op) {
    return std::to_string(static_cast<int>(op.StencilFailOp)) + "," +
           std::to_string(static_cast<int>(op.StencilDepthFailOp)) + "," +
           std::to_string(static_cast<int>(op.StencilPassOp)) + "," +
           std::to_string(static_cast<int>(op.StencilFunc));
}

std::optional<HostPipeline> refuse(const char* reason, const std::vector<std::shared_ptr<Shader>>& shaders) {
    static std::mutex mutex;
    static std::vector<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (std::find(seen.begin(), seen.end(), reason) == seen.end()) {
            seen.emplace_back(reason);
            const std::string text = shaders_text(shaders);
            G6SHIM_WARN("d3d12_xs: no host pipeline: %s [%s] (logged once)", reason, text.c_str());
        }
    }
    return std::nullopt;
}

struct RootSignature {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::vector<std::uint8_t> blob;
};

std::optional<RootSignature> make_root_signature(ID3D12Device* device,
                                                  std::span<const D3D12_ROOT_PARAMETER1> parameters,
                                                  std::span<const D3D12_STATIC_SAMPLER_DESC> samplers,
                                                  bool compute) {
    if (device == nullptr) return std::nullopt;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    desc.Desc_1_1.pParameters = parameters.data();
    desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.Desc_1_1.pStaticSamplers = samplers.data();
    desc.Desc_1_1.Flags = (compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                                   : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) |
                          D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                          D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    const HRESULT serialized = ::D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (FAILED(serialized)) {
        if (error != nullptr) {
            const auto* text = static_cast<const char*>(error->GetBufferPointer());
            G6SHIM_WARN("d3d12_xs: root signature: %.*s", static_cast<int>(error->GetBufferSize()), text);        }
        return std::nullopt;
    }
    if (blob == nullptr) return std::nullopt;
    RootSignature result;
    const auto* bytes = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
    result.blob.assign(bytes, bytes + blob->GetBufferSize());
    if (FAILED(device->CreateRootSignature(0, bytes, blob->GetBufferSize(), IID_PPV_ARGS(&result.root))))
        return std::nullopt;
    return result;
}

std::vector<std::uint8_t> bytecode_bytes(const D3D12_SHADER_BYTECODE& code) {
    if (code.pShaderBytecode == nullptr) return {};
    const auto* bytes = static_cast<const std::uint8_t*>(code.pShaderBytecode);
    return std::vector<std::uint8_t>(bytes, bytes + code.BytecodeLength);
}

#pragma warning(push)
#pragma warning(disable : 4324)
template <typename T>
struct alignas(8) Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};
#pragma warning(pop)

static_assert(sizeof(Subobject<ID3D12RootSignature*>) == 16);
static_assert(sizeof(Subobject<D3D12_SHADER_BYTECODE>) == 24);
static_assert(sizeof(Subobject<D3D12_BLEND_DESC>) == 336);
static_assert(sizeof(Subobject<std::uint32_t>) == 8);
static_assert(sizeof(Subobject<D3D12_RASTERIZER_DESC>) == 48);
static_assert(sizeof(Subobject<D3D12_DEPTH_STENCIL_DESC>) == 56);
static_assert(sizeof(Subobject<D3D12_RT_FORMAT_ARRAY>) == 40);
static_assert(sizeof(Subobject<DXGI_FORMAT>) == 8);
static_assert(sizeof(Subobject<DXGI_SAMPLE_DESC>) == 16);

struct MeshStream {
    Subobject<ID3D12RootSignature*> root;
    Subobject<D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_BLEND_DESC> blend;
    Subobject<std::uint32_t> mask;
    Subobject<D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<DXGI_FORMAT> dsv;
    Subobject<DXGI_SAMPLE_DESC> samples;
};
static_assert(sizeof(MeshStream) % 8 == 0);

HRESULT make_mesh_pipeline(ID3D12Device* device, ID3D12RootSignature* root, D3D12_SHADER_BYTECODE ms,
                           D3D12_SHADER_BYTECODE ps, const GraphicsState& graphics, const std::string& name,
                           ID3D12PipelineState** out) {
    MeshStream stream{
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, ms},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, graphics.blend},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, graphics.sample_mask},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, graphics.rasterizer},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, graphics.depth},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, graphics.targets},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, graphics.depth_format},
        {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, graphics.samples},
    };
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(MeshStream), &stream};
    Microsoft::WRL::ComPtr<ID3D12Device2> device2;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2)))) return E_NOINTERFACE;
    return pipeline_library::pipeline(
        name,
        [&](ID3D12PipelineLibrary1* library, const wchar_t* wide_name) {
            return library->LoadPipeline(wide_name, &desc, IID_PPV_ARGS(out));
        },
        [&](ID3D12PipelineState** state) { return device2->CreatePipelineState(&desc, IID_PPV_ARGS(state)); },
        out);
}

}  // namespace

GraphicsState::GraphicsState() {
    D3D12_RENDER_TARGET_BLEND_DESC target{};
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ZERO;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL);
    for (D3D12_RENDER_TARGET_BLEND_DESC& each : blend.RenderTarget) each = target;
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    rasterizer.DepthClipEnable = TRUE;
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depth.StencilReadMask = 0xFF;
    depth.StencilWriteMask = 0xFF;
    D3D12_DEPTH_STENCILOP_DESC keep{};
    keep.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    keep.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    keep.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    keep.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.FrontFace = keep;
    depth.BackFace = keep;
}

void GraphicsState::read(std::uint32_t kind, const std::uint8_t* at, std::size_t available) {
    if (at == nullptr || available < 4) return;
    const std::uint8_t* data = at + 4;
    const std::size_t bytes = available - 4;
    const auto copy_into = [data, bytes](void* destination, std::size_t size) {
        if (bytes < size) return false;
        std::memcpy(destination, data, size);
        return true;
    };
    switch (kind) {
    case 8: copy_into(&blend, sizeof blend); break;
    case 9: copy_into(&sample_mask, sizeof sample_mask); break;
    case 10: copy_into(&rasterizer, sizeof rasterizer); break;
    case 11: copy_into(&depth, sizeof depth); break;
    case 21: {
        D3D12_DEPTH_STENCIL_DESC1 extended{};
        if (!copy_into(&extended, sizeof extended)) return;
        depth.DepthEnable = extended.DepthEnable;
        depth.DepthWriteMask = extended.DepthWriteMask;
        depth.DepthFunc = extended.DepthFunc;
        depth.StencilEnable = extended.StencilEnable;
        depth.StencilReadMask = extended.StencilReadMask;
        depth.StencilWriteMask = extended.StencilWriteMask;
        depth.FrontFace = extended.FrontFace;
        depth.BackFace = extended.BackFace;
        break;
    }
    case 12: {
        if (available < 8 + sizeof(D3D12_INPUT_LAYOUT_DESC)) return;
        D3D12_INPUT_LAYOUT_DESC layout{};
        std::memcpy(&layout, at + 8, sizeof layout);
        if (layout.pInputElementDescs == nullptr) return;
        std::uint32_t count = layout.NumElements;
        if (count > kMaxInputElements) {
            G6SHIM_WARN("d3d12_xs: pipeline input layout of %u elements; the rest is ignored", count);
            count = kMaxInputElements;
        }
        inputs.clear();
        inputs.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& element = layout.pInputElementDescs[i];
            if (element.SemanticName == nullptr) break;
            std::size_t length = 0;
            while (length < kMaxSemanticName && element.SemanticName[length] != '\0') ++length;
            if (length == kMaxSemanticName) {
                G6SHIM_WARN("d3d12_xs: pipeline input element with an unterminated semantic name");
                break;
            }
            InputElement read{};
            read.name.assign(element.SemanticName, length);
            read.index = element.SemanticIndex;
            read.format = resource::pc_format(static_cast<std::uint32_t>(element.Format));
            read.slot = element.InputSlot;
            read.offset = element.AlignedByteOffset;
            read.classification = static_cast<std::uint32_t>(element.InputSlotClass);
            read.step_rate = element.InstanceDataStepRate;
            inputs.push_back(std::move(read));
        }
        break;
    }
    case 13: copy_into(&strip_cut, sizeof strip_cut); break;
    case 14: copy_into(&topology, sizeof topology); break;
    case 15: {
        D3D12_RT_FORMAT_ARRAY read{};
        if (!copy_into(&read, sizeof read)) return;
        for (DXGI_FORMAT& format : read.RTFormats)
            format = static_cast<DXGI_FORMAT>(resource::pc_format(static_cast<std::uint32_t>(format)));
        targets = read;
        break;
    }
    case 16: {
        std::uint32_t format = 0;
        if (!copy_into(&format, sizeof format)) return;
        depth_format = static_cast<DXGI_FORMAT>(resource::pc_format(format));
        break;
    }
    case 17: copy_into(&samples, sizeof samples); break;
    default: break;
    }
}

std::string GraphicsState::describe() const {
    std::string out = blend_text(blend);
    out += "mask " + std::to_string(sample_mask) + " raster ";
    out += std::to_string(static_cast<int>(rasterizer.FillMode)) + "," +
           std::to_string(static_cast<int>(rasterizer.CullMode)) + "," +
           std::to_string(rasterizer.FrontCounterClockwise) + "," + std::to_string(rasterizer.DepthBias) + "," +
           std::to_string(rasterizer.DepthBiasClamp) + "," + std::to_string(rasterizer.SlopeScaledDepthBias) +
           "," + std::to_string(rasterizer.DepthClipEnable) + "," + std::to_string(rasterizer.MultisampleEnable) +
           "," + std::to_string(rasterizer.AntialiasedLineEnable) + "," +
           std::to_string(rasterizer.ForcedSampleCount) + "," +
           std::to_string(static_cast<int>(rasterizer.ConservativeRaster));
    out += " depth ";
    out += std::to_string(depth.DepthEnable) + "," + std::to_string(static_cast<int>(depth.DepthWriteMask)) + "," +
           std::to_string(static_cast<int>(depth.DepthFunc)) + "," + std::to_string(depth.StencilEnable) + "," +
           std::to_string(depth.StencilReadMask) + "," + std::to_string(depth.StencilWriteMask) + "," +
           op_text(depth.FrontFace) + "," + op_text(depth.BackFace);
    out += " inputs ";
    for (const InputElement& element : inputs) {
        out += "\"" + element.name + "\" " + std::to_string(element.index) + " " + std::to_string(element.format) +
               " " + std::to_string(element.slot) + " " + std::to_string(element.offset) + " " +
               std::to_string(element.classification) + " " + std::to_string(element.step_rate) + ";";
    }
    out += " strip " + std::to_string(static_cast<int>(strip_cut)) + " topology " +
           std::to_string(static_cast<int>(topology)) + " targets " +
           std::to_string(targets.NumRenderTargets);
    for (const DXGI_FORMAT format : targets.RTFormats) out += "," + std::to_string(static_cast<int>(format));
    out += " dsv " + std::to_string(static_cast<int>(depth_format)) + " samples " +
           std::to_string(samples.Count) + "," + std::to_string(samples.Quality);
    return out;
}

std::uint32_t GraphicsState::slots() const {
    std::uint32_t mask = 0;
    for (const InputElement& element : inputs) mask |= 1u << std::min<std::uint32_t>(element.slot, 31);
    return mask;
}

std::vector<D3D12_INPUT_ELEMENT_DESC> GraphicsState::element_descs() const {
    std::vector<D3D12_INPUT_ELEMENT_DESC> out;
    out.reserve(inputs.size());
    for (const InputElement& element : inputs) {
        D3D12_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName = element.name.c_str();
        desc.SemanticIndex = element.index;
        desc.Format = static_cast<DXGI_FORMAT>(element.format);
        desc.InputSlot = element.slot;
        desc.AlignedByteOffset = element.offset;
        desc.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(element.classification);
        desc.InstanceDataStepRate = element.step_rate;
        out.push_back(desc);
    }
    return out;
}

const Translated* Stage::translated() const { return shader ? shader->translated() : nullptr; }

std::optional<HostPipeline> HostPipeline::make(const std::vector<std::shared_ptr<Shader>>& shaders,
                                               const GraphicsState* graphics) {
    const auto find = [&shaders](const char* stage) -> std::shared_ptr<Shader> {
        for (const auto& shader : shaders)
            if (shader->stage == stage) return shader;
        return nullptr;
    };
    const bool compute = shaders.size() == 1 && shaders.front()->stage == "compute";
    if (!compute) {
        for (const auto& shader : shaders)
            if (shader->stage != "vertex" && shader->stage != "pixel" && shader->stage != "mesh" &&
                shader->stage != "geometry")
                return refuse("only vertex, mesh, pixel, geometry and compute stages are translated", shaders);
    }
    const std::shared_ptr<Shader> mesh = find("mesh");
    const std::shared_ptr<Shader> geometry = find("geometry");
    if (mesh && geometry) return refuse("a mesh and geometry shader together", shaders);
    std::vector<std::pair<std::shared_ptr<Shader>, D3D12_SHADER_VISIBILITY>> chosen;
    if (compute) {
        chosen.emplace_back(shaders.front(), D3D12_SHADER_VISIBILITY_ALL);
    } else {
        const std::shared_ptr<Shader> first = mesh ? mesh : find("vertex");
        if (!first) return refuse("no vertex or mesh shader", shaders);
        chosen.emplace_back(first, mesh ? D3D12_SHADER_VISIBILITY_MESH : D3D12_SHADER_VISIBILITY_VERTEX);
        if (geometry) chosen.emplace_back(geometry, D3D12_SHADER_VISIBILITY_GEOMETRY);
        if (const std::shared_ptr<Shader> pixel = find("pixel"); pixel)
            chosen.emplace_back(pixel, D3D12_SHADER_VISIBILITY_PIXEL);
    }
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    if (mesh && !gpu->mesh_shaders())
        return refuse("the host GPU has no mesh shaders (or no shader model 6.6)", shaders);
    for (const auto& entry : chosen)
        if (entry.first->translated() == nullptr) return std::nullopt;

    std::vector<D3D12_ROOT_PARAMETER1> parameters;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    std::vector<Stage> bound;
    for (const auto& [shader, visibility] : chosen) {
        const Translated* translated = shader->translated();
        const auto& host_shader = translated->host;
        Stage stage;
        stage.shader = shader;
        const std::uint32_t dwords = host_shader.user_data_dwords();
        if (dwords > 0) {
            stage.user_data = std::make_pair(static_cast<std::uint32_t>(parameters.size()), dwords);
            D3D12_ROOT_PARAMETER1 parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameter.Constants.ShaderRegister = 0;
            parameter.Constants.RegisterSpace = g6shim::shader::emit::kUserDataSpace;
            parameter.Constants.Num32BitValues = dwords;
            parameter.ShaderVisibility = visibility;
            parameters.push_back(parameter);
        }
        for (const std::uint8_t slot : host_shader.root_cbvs) {
            stage.root_cbvs.emplace_back(slot, static_cast<std::uint32_t>(parameters.size()));
            D3D12_ROOT_PARAMETER1 parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor.ShaderRegister = slot;
            parameter.Descriptor.RegisterSpace = g6shim::shader::emit::kRootCbvSpace;
            parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            parameter.ShaderVisibility = visibility;
            parameters.push_back(parameter);
        }
        for (const std::uint8_t slot : host_shader.root_srvs) {
            stage.root_srvs.emplace_back(slot, static_cast<std::uint32_t>(parameters.size()));
            D3D12_ROOT_PARAMETER1 parameter{};
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameter.Descriptor.ShaderRegister = slot;
            parameter.Descriptor.RegisterSpace = g6shim::shader::emit::kRootCbvSpace;
            parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            parameter.ShaderVisibility = visibility;
            parameters.push_back(parameter);
        }
        for (std::size_t n = 0; n < host_shader.static_samplers.size(); ++n) {
            const g6shim::shader::emit::StaticSampler& source = host_shader.static_samplers[n];
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = static_cast<D3D12_FILTER>(source.filter);
            sampler.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[0]);
            sampler.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[1]);
            sampler.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(source.address[2]);
            sampler.MipLODBias = source.mip_lod_bias;
            sampler.MaxAnisotropy = source.max_anisotropy;
            sampler.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(source.comparison);
            sampler.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(source.border);
            sampler.MinLOD = source.min_lod;
            sampler.MaxLOD = source.max_lod;
            sampler.ShaderRegister = static_cast<UINT>(n);
            sampler.RegisterSpace = g6shim::shader::emit::kSamplerSpace;
            sampler.ShaderVisibility = visibility;
            samplers.push_back(sampler);
        }
        bound.push_back(std::move(stage));
    }
    const std::optional<RootSignature> root =
        make_root_signature(gpu->device(), parameters, samplers, compute);
    if (!root) {
        const std::string text = shaders_text(shaders);
        G6SHIM_WARN("d3d12_xs: host root signature refused [%s]", text.c_str());
        return std::nullopt;
    }
    const auto code = [&chosen](std::size_t index) {
        D3D12_SHADER_BYTECODE out{};
        if (index < chosen.size()) {
            const Translated* translated = chosen[index].first->translated();
            out.pShaderBytecode = translated->host.dxil.data();
            out.BytecodeLength = translated->host.dxil.size();
        }
        return out;
    };
    const std::string state = graphics != nullptr ? graphics->describe() : std::string();
    const std::string text = shaders_text(shaders);
    if (compute) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root->root.Get();
        desc.CS = code(0);
        const std::vector<std::uint8_t> compute_code = bytecode_bytes(desc.CS);
        const std::array<std::span<const std::uint8_t>, 1> codes{compute_code};
        const std::string name = pipeline_library::name("compute", root->blob, codes, "");
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        const HRESULT made = pipeline_library::pipeline(
            name,
            [&](ID3D12PipelineLibrary1* library, const wchar_t* wide_name) {
                return library->LoadComputePipeline(wide_name, &desc, IID_PPV_ARGS(pso.GetAddressOf()));
            },
            [&](ID3D12PipelineState** out) {
                return gpu->device()->CreateComputePipelineState(&desc, IID_PPV_ARGS(out));
            },
            pso.GetAddressOf());
        if (FAILED(made) || pso == nullptr) {
            G6SHIM_WARN("d3d12_xs: host pipeline refused [%s]", text.c_str());
            gpu->log_messages();
            return std::nullopt;
        }
        G6SHIM_INFO("d3d12_xs: host compute pipeline [%s]", text.c_str());
        host::name(pso.Get(), text);
        HostPipeline made_pipeline;
        made_pipeline.compute = true;
        made_pipeline.pso = std::move(pso);
        made_pipeline.root = root->root;
        made_pipeline.stages = std::move(bound);
        made_pipeline.name = text;
        return made_pipeline;
    }
    if (graphics == nullptr) return refuse("graphics state not kept (plain description)", shaders);
    if (mesh) {
        const std::vector<std::uint8_t> mesh_code = bytecode_bytes(code(0));
        const std::vector<std::uint8_t> pixel_code = bytecode_bytes(code(1));
        const std::array<std::span<const std::uint8_t>, 2> codes{mesh_code, pixel_code};
        const std::string name = pipeline_library::name("mesh", root->blob, codes, state);
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        const HRESULT made = make_mesh_pipeline(gpu->device(), root->root.Get(), code(0), code(1), *graphics,
                                                name, pso.GetAddressOf());
        if (FAILED(made) || pso == nullptr) {
            G6SHIM_WARN("d3d12_xs: host mesh pipeline refused [%s]", text.c_str());
            gpu->log_messages();
            return std::nullopt;
        }
        G6SHIM_INFO("d3d12_xs: host mesh pipeline [%s]", text.c_str());
        host::name(pso.Get(), text);
        HostPipeline made_pipeline;
        made_pipeline.mesh = true;
        made_pipeline.pso = std::move(pso);
        made_pipeline.root = root->root;
        made_pipeline.stages = std::move(bound);
        made_pipeline.name = text;
        return made_pipeline;
    }
    const std::vector<D3D12_INPUT_ELEMENT_DESC> elements = graphics->element_descs();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root->root.Get();
    const auto bytecode = [&](D3D12_SHADER_VISIBILITY visibility) {
        D3D12_SHADER_BYTECODE out{};
        for (const auto& entry : chosen) {
            if (entry.second != visibility) continue;
            const Translated* translated = entry.first->translated();
            out.pShaderBytecode = translated->host.dxil.data();
            out.BytecodeLength = translated->host.dxil.size();
            break;
        }
        return out;
    };
    desc.VS = bytecode(D3D12_SHADER_VISIBILITY_VERTEX);
    desc.GS = bytecode(D3D12_SHADER_VISIBILITY_GEOMETRY);
    desc.PS = bytecode(D3D12_SHADER_VISIBILITY_PIXEL);
    desc.BlendState = graphics->blend;
    desc.SampleMask = graphics->sample_mask;
    desc.RasterizerState = graphics->rasterizer;
    desc.DepthStencilState = graphics->depth;
    desc.InputLayout.pInputElementDescs = elements.data();
    desc.InputLayout.NumElements = static_cast<UINT>(elements.size());
    desc.IBStripCutValue = graphics->strip_cut;
    const std::uint32_t topology = static_cast<std::uint32_t>(graphics->topology);
    desc.PrimitiveTopologyType = (topology == kRectangles || topology == kQuads) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                                                                  : graphics->topology;
    desc.NumRenderTargets = std::min<std::uint32_t>(graphics->targets.NumRenderTargets, 8);
    for (std::size_t i = 0; i < 8; ++i) desc.RTVFormats[i] = graphics->targets.RTFormats[i];
    desc.DSVFormat = graphics->depth_format;
    desc.SampleDesc = graphics->samples;
    const std::vector<std::uint8_t> vertex_code = bytecode_bytes(desc.VS);
    const std::vector<std::uint8_t> geometry_code = bytecode_bytes(desc.GS);
    const std::vector<std::uint8_t> pixel_code = bytecode_bytes(desc.PS);
    const std::array<std::span<const std::uint8_t>, 3> codes{vertex_code, geometry_code, pixel_code};
    const std::string name = pipeline_library::name("graphics", root->blob, codes, state);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    const HRESULT made = pipeline_library::pipeline(
        name,
        [&](ID3D12PipelineLibrary1* library, const wchar_t* wide_name) {
            return library->LoadGraphicsPipeline(wide_name, &desc, IID_PPV_ARGS(pso.GetAddressOf()));
        },
        [&](ID3D12PipelineState** out) {
            return gpu->device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
        },
        pso.GetAddressOf());
    if (FAILED(made) || pso == nullptr) {
        G6SHIM_WARN("d3d12_xs: host pipeline refused [%s]", text.c_str());
        gpu->log_messages();
        return std::nullopt;
    }
    G6SHIM_INFO("d3d12_xs: host pipeline [%s]", text.c_str());
    host::name(pso.Get(), text);
    G6SHIM_TRACE("d3d12_xs: host graphics state [%s] dsv %d targets %u", state.c_str(),
                 static_cast<int>(graphics->depth_format), graphics->targets.NumRenderTargets);
    HostPipeline made_pipeline;
    made_pipeline.pso = std::move(pso);
    made_pipeline.root = root->root;
    made_pipeline.stages = std::move(bound);
    made_pipeline.name = text;
    made_pipeline.slots = graphics->slots();
    std::array<std::uint32_t, 32> next{};
    for (const InputElement& element : graphics->inputs) {
        Input input;
        input.slot = std::min<std::uint32_t>(element.slot, 31);
        input.format = element.format;
        input.per_instance = element.classification == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        const std::size_t index = std::min<std::size_t>(input.slot, 31);
        input.offset = element.offset == static_cast<std::uint32_t>(D3D12_APPEND_ALIGNED_ELEMENT) ? next[index]
                                                                                                 : element.offset;
        const auto found = resource::element(input.format);
        next[index] = input.offset + static_cast<std::uint32_t>(found ? found->first : 0);
        made_pipeline.inputs.push_back(input);
    }
    return made_pipeline;
}

}  // namespace g6shim::d3d12_xs::pipeline_host
