// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace g6shim::d3d12_xs {

namespace resource {
struct ResourceDesc;
}

namespace later {

struct HostTexture;

HRESULT create_command_queue(Object* device, const void* desc, const Iid* iid, Object** out);
HRESULT create_command_queue_x(Object* device, const void* desc, std::uint32_t flags, const Iid* iid, Object** out);
HRESULT create_command_allocator(Object* device, std::uint32_t type, const Iid* iid, Object** out);
HRESULT create_command_list(Object* device, std::uint32_t node_mask, std::uint32_t type, Object* allocator,
                            Object* state, const Iid* iid, Object** out);
HRESULT create_command_list_x(Object* device, std::uint32_t node_mask, std::uint32_t type, Object* allocator,
                              Object* state, const Iid* iid, Object** out);
HRESULT create_graphics_pipeline(Object* device, const void* desc, const Iid* iid, Object** out);
HRESULT create_compute_pipeline(Object* device, const void* desc, const Iid* iid, Object** out);
HRESULT create_compute_pipeline_x(Object* device, const void* desc, std::uint32_t flags, const Iid* iid,
                                  Object** out);
HRESULT create_stream_pipeline(Object* device, const void* desc, const Iid* iid, Object** out);

std::shared_ptr<HostTexture> create_host_texture(const resource::ResourceDesc& desc, std::uint32_t state,
                                                 const void* clear);
std::optional<std::uint32_t> host_texture_format(Object* resource);
std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* resource, std::uint32_t wanted);
void texture_forget(Object* resource);
void shadow_register(Object* resource, const void* address, std::uint64_t bytes);
void shadow_unregister(Object* resource, const void* address);
void buffer_copy_to_host(Object* resource);

void descriptor_mirror(std::uint64_t handle);
void descriptor_heap_created(Object* heap, std::uint64_t start, std::uint64_t size, bool shader_visible);
void descriptor_heap_dropped(Object* heap);
void descriptor_release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count);

void title_memory_remapped(const void* at, std::size_t bytes);

}  // namespace later

}  // namespace g6shim::d3d12_xs
