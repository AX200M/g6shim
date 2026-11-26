// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.hpp"

#include "frame.hpp"
#include "pipeline_bridge.hpp"
#include "pipeline_library.hpp"
#include "pipeline_make.hpp"
#include "shader_cache.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/dxbc.hpp>
#include <g6shim/shader/emit/container.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <span>
#include <utility>

namespace g6shim::d3d12_xs {

namespace {

constexpr std::array<const char*, 5> kGraphicsStages{"vertex", "pixel", "domain", "hull", "geometry"};
constexpr std::size_t kMaxContainerBytes = 256u << 20;
constexpr std::size_t kMaxStreamBytes = 16u << 20;

struct SubobjectInfo {
    std::size_t size;
    const char* stage;
};

std::optional<SubobjectInfo> subobject(std::uint32_t kind) {
    switch (kind) {
    case 1: return SubobjectInfo{24, "vertex"};
    case 2: return SubobjectInfo{24, "pixel"};
    case 3: return SubobjectInfo{24, "domain"};
    case 4: return SubobjectInfo{24, "hull"};
    case 5: return SubobjectInfo{24, "geometry"};
    case 6: return SubobjectInfo{24, "compute"};
    case 23: return SubobjectInfo{24, "amplification"};
    case 24: return SubobjectInfo{24, "mesh"};
    case 0: return SubobjectInfo{16, nullptr};
    case 12:
    case 19: return SubobjectInfo{24, nullptr};
    case 7:
    case 15: return SubobjectInfo{40, nullptr};
    case 8: return SubobjectInfo{336, nullptr};
    case 10: return SubobjectInfo{48, nullptr};
    case 11: return SubobjectInfo{56, nullptr};
    case 17: return SubobjectInfo{16, nullptr};
    case 21: return SubobjectInfo{64, nullptr};
    case 22: return SubobjectInfo{32, nullptr};
    case 9:
    case 13:
    case 14:
    case 16:
    case 18:
    case 20: return SubobjectInfo{8, nullptr};
    default: return std::nullopt;
    }
}

std::string digest_text(const std::array<std::uint8_t, 16>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(kDigits[digest[i] >> 4]);
        out.push_back(kDigits[digest[i] & 0x0F]);
    }
    return out;
}

std::mutex g_shaders_mutex;
std::map<std::array<std::uint8_t, 16>, std::shared_ptr<Shader>> g_shaders;

std::shared_ptr<Shader> shared_shader(const char* stage, std::span<const std::uint8_t> bytes) {
    const std::array<std::uint8_t, 16> digest = g6shim::shader::emit::hash(bytes);
    std::lock_guard<std::mutex> lock(g_shaders_mutex);
    const auto found = g_shaders.find(digest);
    if (found != g_shaders.end()) return found->second;
    auto shader = std::make_shared<Shader>(stage, std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                                           digest_text(digest));
    g_shaders.emplace(digest, shader);
    return shader;
}

std::shared_ptr<Shader> copy_shader(const char* stage, const pipeline::Bytecode& code) {
    if (code.code == nullptr || code.size == 0) return nullptr;
    if (code.size > kMaxContainerBytes) {
        G6SHIM_WARN("d3d12_xs: a %s shader of %zu bytes is ignored", stage, code.size);
        return nullptr;
    }
    return shared_shader(stage, std::span<const std::uint8_t>(code.code, code.size));
}

std::array<const void*, 24> make_functions();

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kPipeline, iid, is_iid(iid, pipeline::kIids) ? object : nullptr, out);
}

std::uint32_t release_pipeline(Object* object) { return release_boxed<Pipeline>(object); }

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> functions = make_functions();
    return functions;
}

std::array<const void*, 24> make_functions() {
    std::array<const void*, 24> functions = d3d12_object(missing_table<kPipeline, 24>());
    functions[0] = reinterpret_cast<const void*>(&query);
    functions[2] = reinterpret_cast<const void*>(&release_pipeline);
    functions[7] = reinterpret_cast<const void*>(&parent_device);
    return functions;
}

HRESULT hand_out_pipeline(std::vector<std::shared_ptr<Shader>> shaders,
                          std::shared_ptr<pipeline_host::GraphicsState> graphics, const Iid* iid, Object** out) {
    G6SHIM_TRACE("d3d12_xs: pipeline state %s", shaders_text(shaders).c_str());
    auto pending = std::make_shared<make::Pending>(shaders, std::move(graphics));
    make::early(pending);
    auto object = std::make_unique<Pipeline>(std::move(shaders), std::move(pending));
    return hand_out(kPipeline, std::move(object), iid, pipeline::kIids, out);
}

}  // namespace

Shader::Shader(std::string stage_name, std::vector<std::uint8_t> container_bytes, std::string content_hash)
    : stage(std::move(stage_name)), hash(std::move(content_hash)), container(std::move(container_bytes)) {}

const Translated* Shader::translated() const {
    std::call_once(once_, [this] {
        std::optional<g6shim::shader::emit::HostShader> host = shader_cache::host_shader(container);
        if (!host) {
            G6SHIM_WARN("d3d12_xs: %s shader %s does not translate; its pipelines' draws are dropped", stage.c_str(),
                        hash.c_str());
            return;
        }
        Translated done;
        try {
            const g6shim::shader::Container parsed = g6shim::shader::Container::parse(container);
            const g6shim::shader::Part* part = parsed.part(g6shim::shader::kDxil);
            if (part == nullptr) part = parsed.part(g6shim::shader::kAccc);
            if (part != nullptr) {
                for (const g6shim::shader::NativeProgram& program : g6shim::shader::native_programs(part->data)) {
                    if (program.stage == g6shim::shader::HwStage::Vs ||
                        program.stage == g6shim::shader::HwStage::Ps ||
                        program.stage == g6shim::shader::HwStage::Ms ||
                        program.stage == g6shim::shader::HwStage::Cs ||
                        program.stage == g6shim::shader::HwStage::Gs) {
                        done.used = program.used;
                        break;
                    }
                }
            }
        } catch (const std::exception& error) {
            G6SHIM_WARN("d3d12_xs: %s shader %s: %s", stage.c_str(), hash.c_str(), error.what());
            return;
        }
        done.host = std::move(*host);
        G6SHIM_INFO("d3d12_xs: shader translated (%s %s, %zu user data, %zu samplers)", stage.c_str(), hash.c_str(),
                    done.host.user_data.size(), done.host.samplers.size());
        translated_ = std::move(done);
    });
    return translated_ ? &*translated_ : nullptr;
}

Pipeline::Pipeline(std::vector<std::shared_ptr<Shader>> pipeline_shaders, std::shared_ptr<make::Pending> pending)
    : Object(table().data()), shaders(std::move(pipeline_shaders)), host(std::move(pending)) {}

namespace pipeline {

Pipeline* as_pipeline(Object* object) { return static_cast<Pipeline*>(object); }

HRESULT create_graphics(const void* desc_pointer, const Iid* iid, Object** out) {
    if (desc_pointer == nullptr) return E_INVALIDARG;
    const auto& desc = *static_cast<const GraphicsHead*>(desc_pointer);
    std::vector<std::shared_ptr<Shader>> shaders;
    for (std::size_t i = 0; i < kGraphicsStages.size(); ++i) {
        if (std::shared_ptr<Shader> shader = copy_shader(kGraphicsStages[i], desc.stages[i]); shader)
            shaders.push_back(std::move(shader));
    }
    return hand_out_pipeline(std::move(shaders), nullptr, iid, out);
}

HRESULT create_compute(const void* desc_pointer, const Iid* iid, Object** out) {
    if (desc_pointer == nullptr) return E_INVALIDARG;
    const auto& desc = *static_cast<const ComputeHead*>(desc_pointer);
    std::vector<std::shared_ptr<Shader>> shaders;
    if (std::shared_ptr<Shader> shader = copy_shader("compute", desc.stage); shader)
        shaders.push_back(std::move(shader));
    return hand_out_pipeline(std::move(shaders), nullptr, iid, out);
}

HRESULT create_compute_x(const void* desc, std::uint32_t, const void*, const Iid* iid, Object** out) {
    return create_compute(desc, iid, out);
}

HRESULT create_stream(const void* desc_pointer, const Iid* iid, Object** out) {
    if (desc_pointer == nullptr) return E_INVALIDARG;
    const auto& desc = *static_cast<const StreamDesc*>(desc_pointer);
    if (desc.stream == nullptr) return E_INVALIDARG;
    std::size_t size = desc.size;
    if (size > kMaxStreamBytes) {
        G6SHIM_WARN("d3d12_xs: pipeline stream of %zu bytes; only the first %zu are read", desc.size, kMaxStreamBytes);
        size = kMaxStreamBytes;
    }
    std::vector<std::shared_ptr<Shader>> shaders;
    auto graphics = std::make_shared<pipeline_host::GraphicsState>();
    std::size_t at = 0;
    while (at + 8 <= size) {
        std::uint32_t kind = 0;
        std::memcpy(&kind, desc.stream + at, sizeof kind);
        const std::optional<SubobjectInfo> info = subobject(kind);
        if (!info) {
            G6SHIM_WARN("d3d12_xs: pipeline subobject of unknown kind %u at %zu; the rest is ignored", kind, at);
            break;
        }
        if (at + info->size > size) {
            G6SHIM_WARN("d3d12_xs: pipeline subobject %u at %zu runs past the stream", kind, at);
            break;
        }
        if (info->stage != nullptr) {
            Bytecode code{};
            std::memcpy(&code, desc.stream + at + 8, sizeof code);
            if (std::shared_ptr<Shader> shader = copy_shader(info->stage, code); shader)
                shaders.push_back(std::move(shader));
        }
        graphics->read(kind, desc.stream + at, info->size);
        at += info->size;
    }
    return hand_out_pipeline(std::move(shaders), std::move(graphics), iid, out);
}

pipeline_host::HostPipeline* host_pipeline(Object* object) {
    if (object == nullptr) return nullptr;
    return make::ready(as_pipeline(object)->host);
}

void bound(Object* object) {
    if (object == nullptr) return;
    Pipeline* found = as_pipeline(object);
    if (found->bound.exchange(true)) return;
    const std::uint64_t frame = frame::current_frame();
    for (const auto& shader : found->shaders)
        G6SHIM_INFO("d3d12_xs: pipeline %p first bind, frame %llu: %s %s (%zu bytes)", object,
                    static_cast<unsigned long long>(frame), shader->stage.c_str(), shader->hash.c_str(),
                    shader->container.size());
}

std::uint64_t making() { return make::waiting(); }

void use_cache_directory(const std::filesystem::path& root) {
    shader_cache::use_directory(root / L"shaders");
    pipeline_library::use_directory(root / L"pipelines");
}

void save_pipeline_cache() { pipeline_library::save(true); }

}  // namespace pipeline

namespace pipeline_bridge {

pipeline_host::HostPipeline* host_pipeline_for(Object* title_pipeline, bool) {
    return pipeline::host_pipeline(title_pipeline);
}

pipeline_host::HostPipeline* host_pipeline_for(Object* title_pipeline) {
    return pipeline::host_pipeline(title_pipeline);
}

void bound(Object* title_pipeline) { pipeline::bound(title_pipeline); }

std::uint64_t making() { return pipeline::making(); }

}  // namespace pipeline_bridge

std::string shaders_text(const std::vector<std::shared_ptr<Shader>>& shaders) {
    std::string out = "[";
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        if (i > 0) out += ", ";
        out += shaders[i]->stage + " " + shaders[i]->hash;
    }
    out += "]";
    return out;
}

}  // namespace g6shim::d3d12_xs
