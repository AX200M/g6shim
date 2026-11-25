// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/shader/emit/emit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::shader_cache {

using Translator = std::function<std::optional<g6shim::shader::emit::HostShader>(std::span<const std::uint8_t>)>;

class Cache {
public:
    static std::optional<Cache> open(const std::filesystem::path& dir);

    const std::filesystem::path& directory() const { return dir_; }
    std::filesystem::path entry_path(const std::array<std::uint8_t, 16>& build,
                                     std::span<const std::uint8_t> container) const;

private:
    explicit Cache(std::filesystem::path dir) : dir_(std::move(dir)) {}

    std::filesystem::path dir_;
};

std::optional<std::filesystem::path> cache_root(const std::filesystem::path& executable,
                                                const std::optional<std::filesystem::path>& override_dir);
std::optional<std::filesystem::path> root_directory();
void use_directory(const std::filesystem::path& dir);

std::optional<g6shim::shader::emit::HostShader> cached(const Cache& cache,
                                                       const std::array<std::uint8_t, 16>& build,
                                                       std::span<const std::uint8_t> container,
                                                       const Translator& translate);

std::optional<g6shim::shader::emit::HostShader> host_shader(std::span<const std::uint8_t> container);

std::pair<std::uint64_t, std::uint64_t> counts();

const std::array<std::uint8_t, 16>& build_fingerprint();

std::vector<std::uint8_t> encode_entry(const g6shim::shader::emit::HostShader& shader);
std::optional<g6shim::shader::emit::HostShader> decode_entry(std::span<const std::uint8_t> bytes);

std::optional<g6shim::shader::emit::HostShader> translate_container(std::span<const std::uint8_t> container);

}  // namespace g6shim::d3d12_xs::shader_cache
