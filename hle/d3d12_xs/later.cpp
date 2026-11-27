// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "later.hpp"

#include "descriptor_host.hpp"
#include "pipeline.hpp"
#include "resource.hpp"
#include "shadow.hpp"
#include "texture.hpp"
#include "watch.hpp"

namespace g6shim::d3d12_xs::later {

HRESULT create_graphics_pipeline(Object*, const void* desc, const Iid* iid, Object** out) {
    return pipeline::create_graphics(desc, iid, out);
}

HRESULT create_compute_pipeline(Object*, const void* desc, const Iid* iid, Object** out) {
    return pipeline::create_compute(desc, iid, out);
}

HRESULT create_compute_pipeline_x(Object*, const void* desc, std::uint32_t, const void*, const Iid* iid,
                                  Object** out) {
    return pipeline::create_compute(desc, iid, out);
}

HRESULT create_stream_pipeline(Object*, const void* desc, const Iid* iid, Object** out) {
    return pipeline::create_stream(desc, iid, out);
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
    shadow::title_memory_changed(reinterpret_cast<std::uint64_t>(at), bytes);
}

}  // namespace g6shim::d3d12_xs::later
