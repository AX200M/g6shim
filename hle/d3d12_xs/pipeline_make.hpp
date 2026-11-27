// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pipeline_host.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace g6shim::d3d12_xs {

struct Shader;

namespace make {

struct Pending {
    Pending(std::vector<std::shared_ptr<Shader>> pipeline_shaders, std::shared_ptr<pipeline_host::GraphicsState> state);

    void make();

    bool queued() const { return queued_.load(std::memory_order_acquire); }
    bool mark_queued() { return !queued_.exchange(true); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    pipeline_host::HostPipeline* result() const;

    std::vector<std::shared_ptr<Shader>> shaders;
    std::shared_ptr<pipeline_host::GraphicsState> graphics;
    std::string key;
    bool screen = false;
    std::atomic<bool> used{false};

private:
    std::atomic<bool> queued_{false};
    std::atomic<bool> done_{false};
    std::once_flag once_;
    mutable std::optional<pipeline_host::HostPipeline> made_;
};

pipeline_host::HostPipeline* ready(const std::shared_ptr<Pending>& pending);
void early(const std::shared_ptr<Pending>& pending);
std::uint64_t waiting();

}  // namespace make

}  // namespace g6shim::d3d12_xs
