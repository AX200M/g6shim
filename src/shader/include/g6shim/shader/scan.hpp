// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6shim/shader/dxil.hpp"

namespace g6shim::shader {

inline constexpr std::size_t kMaxPackedUnpacked = 64 << 20;

struct PackedShaderRecord {
    std::size_t offset = 0;
    bool has_name = false;
    std::string name;
    std::uint8_t kind = 0;
    std::span<const std::uint8_t> packed;
    std::size_t unpacked_size = 0;

    std::vector<std::uint8_t> unpack() const;
};

std::vector<PackedShaderRecord> packed_shader_records(std::span<const std::uint8_t> file);

struct StoredShaderContainer {
    std::size_t offset = 0;
    std::span<const std::uint8_t> bytes;
};

std::vector<StoredShaderContainer> stored_shader_containers(std::span<const std::uint8_t> file);

std::string shader_stage(ShaderKind kind);

struct StageCounts {
    std::uint64_t total = 0;
    std::uint64_t decoded = 0;
    std::uint64_t translated = 0;
    std::uint64_t failed = 0;
};

struct FailureCount {
    std::uint64_t count = 0;
    std::string first;
};

struct ShaderTotals {
    std::uint64_t containers = 0;
    std::uint64_t unique = 0;
    std::uint64_t files = 0;
    std::map<std::string, StageCounts> stages;
    std::map<std::string, FailureCount> failures;
};

struct FileCounts {
    std::uint64_t containers = 0;
    std::uint64_t unique = 0;
};

class ShaderScanner {
public:
    using TranslatedWriter = std::function<void(std::string_view hash, std::span<const std::uint8_t> dxil)>;

    FileCounts add_file(std::span<const std::uint8_t> file);
    const ShaderTotals& totals() const { return totals_; }

    void set_translated_writer(TranslatedWriter writer) { writer_ = std::move(writer); }

    using ContainerVisitor = std::function<void(std::span<const std::uint8_t> container)>;
    void set_container_visitor(ContainerVisitor visitor) { visitor_ = std::move(visitor); }

    using FailureReporter =
        std::function<void(std::span<const std::uint8_t> container, std::string_view stage, std::string_view why)>;
    void set_failure_reporter(FailureReporter reporter) { reporter_ = std::move(reporter); }

private:
    struct Key {
        std::uint64_t first = 0;
        std::uint64_t second = 0;
        std::size_t size = 0;

        friend bool operator==(const Key&, const Key&) = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };

    bool remember(std::span<const std::uint8_t> bytes, std::string& hash);
    void add_failure(const std::string& reason, const std::string& message);

    std::unordered_set<Key, KeyHash> seen_;
    ShaderTotals totals_;
    TranslatedWriter writer_;
    ContainerVisitor visitor_;
    FailureReporter reporter_;
};

}  // namespace g6shim::shader
