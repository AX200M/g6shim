// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "later.hpp"

#include "descriptor_host.hpp"
#include "resource.hpp"
#include "shadow.hpp"
#include "texture.hpp"
#include "watch.hpp"

#include <g6shim/log.hpp>

#include <atomic>

namespace g6shim::d3d12_xs::later {

namespace {

HRESULT unimplemented(const char* what) {
    static std::atomic<std::uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        G6SHIM_WARN("d3d12_xs: %s arrives before the host command lists", what);
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

std::shared_ptr<HostTexture> create_host_texture(const resource::ResourceDesc& desc, std::uint32_t state,
                                                 const void* clear) {
    return texture::create_host_texture(desc, state, clear);
}

std::optional<std::uint32_t> host_texture_format(Object* resource) {
    return texture::host_texture_format(resource);
}

std::optional<std::pair<void*, std::uint32_t>> host_transition(Object* resource, std::uint32_t wanted) {
    return texture::host_transition(resource, wanted);
}

void texture_forget(Object* resource) { texture::forget(resource); }

void shadow_register(Object* resource, const void* address, std::uint64_t bytes) {
    shadow::register_buffer(reinterpret_cast<std::uintptr_t>(resource), reinterpret_cast<std::uintptr_t>(address),
                            bytes);
}

void shadow_unregister(Object* resource, const void* address) {
    shadow::unregister_buffer(reinterpret_cast<std::uintptr_t>(resource),
                              reinterpret_cast<std::uintptr_t>(address));
}

void buffer_copy_to_host(Object* resource) {
    const auto bytes = resource::buffer_bytes(resource);
    if (!bytes) return;
    const auto found = shadow::find(reinterpret_cast<std::uintptr_t>(bytes->first), bytes->second);
    if (!found) return;
    (*found)->update(reinterpret_cast<std::uintptr_t>(bytes->first), static_cast<std::size_t>(bytes->second));
}

void descriptor_mirror(std::uint64_t handle) { descriptor_host::mirror(handle); }

void descriptor_heap_created(Object* heap, std::uint64_t start, std::uint64_t end, bool shader_visible) {
    if (!shader_visible) return;
    descriptor_host::register_heap(heap, start, end);
}

void descriptor_heap_dropped(Object* heap) { descriptor_host::drop_heap(heap); }

void descriptor_release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count) {
    descriptor_host::release_dynamic(heap, first, count);
}

void title_memory_remapped(const void* at, std::size_t bytes) {
    watch::title_memory_remapped(reinterpret_cast<std::uintptr_t>(at), bytes);
}

}  // namespace g6shim::d3d12_xs::later
