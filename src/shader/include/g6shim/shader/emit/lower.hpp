// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "g6shim/shader/emit/emit.hpp"
#include "g6shim/shader/emit/shader.hpp"

namespace g6shim::shader::emit {

enum class Numeric : std::uint8_t {
    Unorm,
    Snorm,
    Uscaled,
    Sscaled,
    Uint,
    Sint,
    Float,
};

struct BufferFormat {
    std::span<const std::uint32_t> bits;
    Numeric numeric = Numeric::Unorm;
};

std::optional<BufferFormat> buffer_format(std::uint8_t format);
std::uint32_t half_shift(std::uint32_t width);

struct Lowered {
    std::vector<std::uint8_t> dxil;
    std::uint64_t cmp_samplers = 0;
    std::vector<std::uint8_t> root_cbvs;
    std::vector<std::uint8_t> root_srvs;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;
};

struct Parts {
    const NativeProgram* native = nullptr;
    const Program* p = nullptr;
    const Structured* st = nullptr;
    const Io* io = nullptr;
    const Resources* res = nullptr;
    std::vector<SigElement> inputs;
    std::vector<SigElement> outputs;
    std::vector<SigElement> prims;
    std::uint32_t mesh_stride = 0;
};

Lowered lower(const Parts& parts);

}  // namespace g6shim::shader::emit
