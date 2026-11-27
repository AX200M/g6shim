// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs {

struct Shader;
struct Translated;

namespace pipeline_host {

struct InputElement {
    std::string name;
    std::uint32_t index = 0;
    std::uint32_t format = 0;
    std::uint32_t slot = 0;
    std::uint32_t offset = 0;
    std::uint32_t classification = 0;
    std::uint32_t step_rate = 0;
};

struct GraphicsState {
    GraphicsState();

    D3D12_BLEND_DESC blend{};
    std::uint32_t sample_mask = UINT32_MAX;
    D3D12_RASTERIZER_DESC rasterizer{};
    D3D12_DEPTH_STENCIL_DESC depth{};
    std::vector<InputElement> inputs;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    D3D12_RT_FORMAT_ARRAY targets{};
    DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC samples{1, 0};

    void read(std::uint32_t kind, const std::uint8_t* at, std::size_t available);
    std::string describe() const;
    bool screen_pass() const;
    std::uint32_t slots() const;
    std::vector<D3D12_INPUT_ELEMENT_DESC> element_descs() const;
};

struct Input {
    std::uint32_t slot = 0;
    std::uint32_t offset = 0;
    std::uint32_t format = 0;
    bool per_instance = false;
};

struct Stage {
    std::shared_ptr<Shader> shader;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> user_data;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> root_cbvs;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> root_srvs;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> descriptor_words;
    bool writes_through_table = false;

    const Translated* translated() const;
};

struct HostPipeline {
    bool compute = false;
    bool mesh = false;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::vector<Stage> stages;
    std::uint32_t slots = 0;
    std::vector<Input> inputs;
    std::string name;

    static std::optional<HostPipeline> make(const std::vector<std::shared_ptr<Shader>>& shaders,
                                            const GraphicsState* graphics);
};

}  // namespace pipeline_host

}  // namespace g6shim::d3d12_xs
