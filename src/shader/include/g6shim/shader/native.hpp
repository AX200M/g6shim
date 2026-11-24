// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace g6shim::shader {

enum class HwStage : std::uint8_t {
    Ls,
    Hs,
    Es,
    Gs,
    Vs,
    Ps,
    Ms,
    Cs,
};

std::uint8_t user_sgpr(HwStage stage, std::uint8_t index);

struct Usage {
    enum class Kind : std::uint8_t { Generic, ClipDistance, Other };

    Kind kind = Kind::Other;
    std::uint8_t raw = 0;

    friend bool operator==(const Usage&, const Usage&) = default;
};

Usage usage_from_raw(std::uint8_t raw);

struct PsInput {
    Usage usage;
    std::uint8_t row = 0;
    std::uint8_t mask = 0;
    std::uint8_t attr = 0;
    bool flat = false;
    bool fp16 = false;
    bool pack2 = false;
    std::uint8_t row2 = 0;

    friend bool operator==(const PsInput&, const PsInput&) = default;
};

struct VsInput {
    Usage usage;
    std::uint8_t row = 0;
    std::uint8_t mask = 0;
    std::uint8_t vgpr = 0;

    friend bool operator==(const VsInput&, const VsInput&) = default;
};

struct VsOutput {
    Usage usage;
    std::uint8_t row = 0;
    std::uint8_t param = 0;
    bool fp16 = false;
    bool pack2 = false;

    friend bool operator==(const VsOutput&, const VsOutput&) = default;
};

struct ExportPatch {
    std::uint8_t mrt = 0;
    std::uint32_t offset = 0;
    std::vector<std::uint32_t> code;

    friend bool operator==(const ExportPatch&, const ExportPatch&) = default;
};

struct ComputeIo {
    std::uint32_t rsrc2 = 0;
    std::array<std::uint32_t, 3> threads{};

    std::uint8_t user_sgprs() const;
    std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>> system_sgprs() const;
    std::uint8_t initialized_sgprs() const;
    std::uint8_t thread_id_vgprs() const;
    std::uint32_t lds_bytes() const;

    friend bool operator==(const ComputeIo&, const ComputeIo&) = default;
};

struct MeshIo {
    std::vector<VsOutput> outputs;
    std::uint32_t clip_cntl = 0;
    std::uint32_t threads = 0;
    std::uint32_t max_vertices = 0;
    std::uint32_t max_primitives = 0;
    std::uint8_t topology = 0;
    std::uint32_t lds_bytes = 0;

    friend bool operator==(const MeshIo&, const MeshIo&) = default;
};

struct VsIo {
    std::vector<VsInput> inputs;
    std::vector<VsOutput> outputs;

    friend bool operator==(const VsIo&, const VsIo&) = default;
};

struct PsIo {
    std::vector<PsInput> inputs;
    std::uint32_t input_addr = 0;
    std::uint32_t db_shader_control = 0;
    std::vector<ExportPatch> patches;

    friend bool operator==(const PsIo&, const PsIo&) = default;
};

using StageIo = std::variant<std::monostate, VsIo, PsIo, ComputeIo, MeshIo>;

struct RootType {
    enum class Kind : std::uint8_t {
        Constants,
        Descriptor,
        Table,
        ImmediateSampler,
        ResourceHeap,
        SamplerHeap,
        Other,
    };

    Kind kind = Kind::Other;
    std::uint32_t raw = 0;

    friend bool operator==(const RootType&, const RootType&) = default;
};

RootType root_type_from_raw(std::uint32_t raw);

struct RootParam {
    RootType root;
    std::uint32_t table_offset_dwords = 0;
    std::uint32_t sgprs = 0;
    std::uint32_t base_register = 0;
    std::uint8_t user_data = 0;
    std::uint8_t slot = 0;

    friend bool operator==(const RootParam&, const RootParam&) = default;
};

struct ResourceType {
    enum class Kind : std::uint8_t { Srv, Sampler, Cbv, Uav, Other };

    Kind kind = Kind::Other;
    std::uint8_t raw = 0;

    friend bool operator==(const ResourceType&, const ResourceType&) = default;
};

ResourceType resource_type_from_raw(std::uint8_t raw);

struct UsedResource {
    ResourceType kind;
    RootType root;
    std::uint32_t table_offset_dwords = 0;
    std::uint32_t lower_bound = 0;
    std::uint32_t stride = 0;
    std::uint8_t dwords = 0;
    std::uint8_t user_data = 0;
    std::uint8_t slot = 0;

    friend bool operator==(const UsedResource&, const UsedResource&) = default;
};

struct NativeProgram {
    HwStage stage = HwStage::Ps;
    bool wave64 = false;
    std::uint16_t vgprs = 0;
    std::vector<RootParam> layout;
    std::vector<UsedResource> used;
    StageIo io;
    std::vector<std::uint8_t> code;

    std::vector<std::uint8_t> patched_code() const;

    friend bool operator==(const NativeProgram&, const NativeProgram&) = default;
};

std::vector<NativeProgram> native_programs(std::span<const std::uint8_t> part);

}  // namespace g6shim::shader
