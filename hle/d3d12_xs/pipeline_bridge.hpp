// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"
#include "pipeline_host.hpp"

#include <cstdint>

namespace g6shim::d3d12_xs::pipeline_bridge {

pipeline_host::HostPipeline* host_pipeline_for(Object* title_pipeline, bool compute);
pipeline_host::HostPipeline* host_pipeline_for(Object* title_pipeline);
void bound(Object* title_pipeline);
std::uint64_t making();

}  // namespace g6shim::d3d12_xs::pipeline_bridge
