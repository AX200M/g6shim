// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"
#include "pipeline_host.hpp"

#include <g6shim/shader/emit/emit.hpp>
#include <g6shim/shader/native.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace g6shim::d3d12_xs {

namespace make {
struct Pending;
}

struct Translated {
    g6shim::shader::emit::HostShader host;
    std::vector<g6shim::shader::UsedResource> used;
};

struct Shader {
    Shader(std::string stage_name, std::vector<std::uint8_t> container_bytes, std::string content_hash);

    const Translated* translated() const;

    std::string stage;
    std::string hash;
    std::vector<std::uint8_t> container;

private:
    mutable std::once_flag once_;
    mutable std::optional<Translated> translated_;
};

struct Pipeline : Object {
    Pipeline(std::vector<std::shared_ptr<Shader>> pipeline_shaders, std::shared_ptr<make::Pending> pending);

    std::array<std::uint32_t, 8> packet{};
    std::vector<std::shared_ptr<Shader>> shaders;
    std::atomic<bool> bound{false};
    std::shared_ptr<make::Pending> host;
};

namespace pipeline {

inline constexpr std::array<Iid, 4> kIids{
    guid(0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}),
    kPageableIids[0],
    kPageableIids[1],
    kPageableIids[2],
};

struct Bytecode {
    const std::uint8_t* code = nullptr;
    std::size_t size = 0;
};
static_assert(sizeof(Bytecode) == 16);

struct GraphicsHead {
    void* root = nullptr;
    std::array<Bytecode, 5> stages{};
};
static_assert(sizeof(GraphicsHead) == 0x58);

struct ComputeHead {
    void* root = nullptr;
    Bytecode stage{};
};
static_assert(sizeof(ComputeHead) == 0x18);

struct StreamDesc {
    std::size_t size = 0;
    const std::uint8_t* stream = nullptr;
};
static_assert(sizeof(StreamDesc) == 0x10);

Pipeline* as_pipeline(Object* object);

HRESULT create_graphics(const void* desc, const Iid* iid, Object** out);
HRESULT create_compute(const void* desc, const Iid* iid, Object** out);
HRESULT create_compute_x(const void* desc, std::uint32_t extensions, const void* extended, const Iid* iid,
                         Object** out);
HRESULT create_stream(const void* desc, const Iid* iid, Object** out);

pipeline_host::HostPipeline* host_pipeline(Object* object);
void bound(Object* object);
std::uint64_t making();

void use_cache_directory(const std::filesystem::path& root);
void save_pipeline_cache();

}  // namespace pipeline

std::string shaders_text(const std::vector<std::shared_ptr<Shader>>& shaders);

}  // namespace g6shim::d3d12_xs
