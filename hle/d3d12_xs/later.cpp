// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "later.hpp"

#include <g6shim/log.hpp>

#include <atomic>

namespace g6shim::d3d12_xs::later {

namespace {

HRESULT unimplemented(const char* what) {
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        G6SHIM_WARN("d3d12_xs: %s arrives before the host GPU", what);
    return E_NOTIMPL;
}

}  // namespace

HRESULT create_command_queue(Object*, const void*, const Iid*, Object**) {
    return unimplemented("command queue creation");
}

HRESULT create_command_queue_x(Object*, const void*, std::uint32_t, const Iid*, Object**) {
    return unimplemented("console command queue creation");
}

HRESULT create_command_allocator(Object*, std::uint32_t, const Iid*, Object**) {
    return unimplemented("command allocator creation");
}

HRESULT create_command_list(Object*, std::uint32_t, std::uint32_t, Object*, Object*, const Iid*, Object**) {
    return unimplemented("command list creation");
}

HRESULT create_command_list_x(Object*, std::uint32_t, std::uint32_t, Object*, Object*, const Iid*, Object**) {
    return unimplemented("console command list creation");
}

HRESULT create_graphics_pipeline(Object*, const void*, const Iid*, Object**) {
    return unimplemented("graphics pipeline creation");
}

HRESULT create_compute_pipeline(Object*, const void*, const Iid*, Object**) {
    return unimplemented("compute pipeline creation");
}

HRESULT create_compute_pipeline_x(Object*, const void*, std::uint32_t, const Iid*, Object**) {
    return unimplemented("console compute pipeline creation");
}

HRESULT create_stream_pipeline(Object*, const void*, const Iid*, Object**) {
    return unimplemented("stream pipeline creation");
}

std::shared_ptr<HostTexture> create_host_texture(const resource::ResourceDesc&, std::uint32_t, const void*) {
    return nullptr;
}

std::optional<std::uint32_t> host_texture_format(Object*) {
    return std::nullopt;
}

std::optional<std::pair<void*, std::uint32_t>> host_transition(Object*, std::uint32_t) {
    return std::nullopt;
}

void texture_forget(Object*) {}

void shadow_register(Object*, const void*, std::uint64_t) {}

void shadow_unregister(Object*, const void*) {}

void buffer_copy_to_host(Object*) {}

void descriptor_mirror(std::uint64_t) {}

void descriptor_heap_created(Object*, std::uint64_t, std::uint64_t, bool) {}

void descriptor_heap_dropped(Object*) {}

void descriptor_release_dynamic(Object*, std::uint32_t, std::uint32_t) {}

void title_memory_remapped(const void* at, std::size_t bytes) {
    G6SHIM_TRACE("d3d12_xs: title memory remapped at %p, %zu bytes", at, bytes);
}

}  // namespace g6shim::d3d12_xs::later
