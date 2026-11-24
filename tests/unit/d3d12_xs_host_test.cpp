// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/resource.hpp"
#include "d3d12_xs/shadow.hpp"
#include "d3d12_xs/texture.hpp"
#include "d3d12_xs/watch.hpp"
#include "xmem/private.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

constexpr DWORD kWriteWatch = 0x0020'0000;

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

host::Host* test_host() {
    static host::Host* instance = [] {
        ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
        return host::host();
    }();
    return instance;
}

struct ExtendedHeap {
    descriptor::HeapDesc desc;
    descriptor::Placement placement;
};

resource::ResourceDesc buffer_desc(std::uint64_t width) {
    resource::ResourceDesc desc{};
    desc.dimension = resource::kBuffer;
    desc.width = width;
    desc.height = 1;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 1;
    desc.sample_count = 1;
    desc.layout = 1;
    return desc;
}

resource::ResourceDesc texture_desc() {
    resource::ResourceDesc desc = buffer_desc(0);
    desc.dimension = 3;
    desc.width = 64;
    desc.height = 64;
    desc.format = 28;
    desc.layout = 0;
    return desc;
}

Object* create_buffer(std::uint64_t width) {
    const resource::HeapProperties properties{1, 0, 0, 0, 0};
    const resource::ResourceDesc desc = buffer_desc(width);
    Object* resource = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &desc, 0, nullptr, &resource::kResourceIids[0],
                                      &resource) == S_OK);
    return resource;
}

}  // namespace

TEST(d3d12_xs_host_batch_checks_a_texture_once_per_submission) {
    host::Batch batch;
    const auto first = std::make_shared<std::array<std::uint8_t, 16>>();
    const auto second = std::make_shared<std::array<std::uint8_t, 16>>();
    CHECK(batch.texture_due(first.get()));
    CHECK(batch.texture_due(second.get()));
    CHECK(!batch.texture_due(first.get()));
    CHECK(!batch.texture_due(first.get()));
    CHECK(!batch.texture_due(second.get()));
    batch.title_written();
    CHECK(batch.texture_due(first.get()));
    CHECK(!batch.texture_due(first.get()));
    host::Batch next;
    CHECK(next.texture_due(first.get()));
}

TEST(d3d12_xs_host_device_runs_lists) {
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    CHECK(gpu->device() != nullptr);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    std::array<std::uint8_t, 256> title{};
    for (std::size_t i = 0; i < title.size(); ++i) title[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const auto upload = list->upload(title.size());
    CHECK(upload.has_value());
    if (upload) {
        std::memset(upload->second, 0, title.size());
        list->defer_fill(upload->second, title.data(), title.size());
        list->close();
        gpu->execute({list.get()});
        CHECK(std::memcmp(upload->second, title.data(), title.size()) == 0);
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
}

TEST(d3d12_xs_host_dynamic_slots_stay_taken_until_released) {
    if (test_host() == nullptr) return;
    auto heap = descriptor_host::HostHeap::create(false, 4);
    if (!heap) return;
    const auto first = heap->dynamic(descriptor_host::kDynamicSlots - 100);
    CHECK(first == 4u);
    const auto second = heap->dynamic(60);
    CHECK(second == 4u + descriptor_host::kDynamicSlots - 100);
    CHECK(!heap->dynamic(50).has_value());
    heap->release(4, 50);
    CHECK(heap->dynamic(50) == std::optional<std::uint32_t>(4));
    CHECK(heap->dynamic(40) == std::optional<std::uint32_t>(4u + descriptor_host::kDynamicSlots - 40));
}

TEST(d3d12_xs_host_null_uavs_keep_the_titles_dimension) {
    const auto dimension = [](std::optional<std::uint32_t> value) {
        return descriptor_host::null_uav_desc(value).ViewDimension;
    };
    const D3D12_UNORDERED_ACCESS_VIEW_DESC three = descriptor_host::null_uav_desc(8);
    CHECK(three.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D);
    CHECK(three.Texture3D.WSize == UINT_MAX);
    const D3D12_UNORDERED_ACCESS_VIEW_DESC array = descriptor_host::null_uav_desc(5);
    CHECK(array.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY);
    CHECK(array.Texture2DArray.ArraySize == 1);
    CHECK(dimension(2) == D3D12_UAV_DIMENSION_TEXTURE1D);
    CHECK(dimension(4) == D3D12_UAV_DIMENSION_TEXTURE2D);
    CHECK(dimension(std::nullopt) == D3D12_UAV_DIMENSION_TEXTURE2D);
    CHECK(dimension(6) == D3D12_UAV_DIMENSION_TEXTURE2D);
}

TEST(d3d12_xs_host_null_3d_uav_leaves_the_device_alive) {
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    auto heap = descriptor_host::HostHeap::create(false, 4);
    if (!heap) return;
    const descriptor::View view =
        descriptor::UnorderedView{0, 0, std::array<std::uint32_t, 10>{28, 8, 0, 0, 0, 0, 0, 0, 0, 0}};
    heap->make_at(0, view);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
}

TEST(d3d12_xs_host_descriptor_heap_mirrors_a_buffer_view) {
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    const ExtendedHeap extended{{0, 4, 1, 0}, {0, 0}};
    Object* heap = nullptr;
    CHECK(descriptor::create_heap(nullptr, &extended.desc, &descriptor::kIids[0], &heap) == S_OK);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    Object* buffer = create_buffer(1024);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    const auto start = static_cast<descriptor::Heap*>(heap)->start;
    const std::array<std::uint32_t, 10> words{42, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 0, 0, 64, 0, 0, 0};
    const std::uint64_t handle = start;
    descriptor::create_srv(nullptr, buffer, words.data(), handle);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
    CHECK(host_heap != nullptr);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    if (host_heap) {
        CHECK(host_heap->count() == 4);
        const auto bytes = resource::buffer_bytes(buffer);
        CHECK(bytes.has_value());
        if (bytes) {
            const auto found = shadow::find(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second);
            CHECK(found.has_value());
            CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
        }
        const auto view = host_heap->mirror(0, handle, true);
        CHECK(view.has_value());
        CHECK(view && std::holds_alternative<descriptor::ShaderView>(*view));
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    CHECK(slot<std::uint32_t (*)(Object*)>(buffer, 0x10)(buffer) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
}

TEST(d3d12_xs_host_shadow_copies_title_buffers) {
    if (test_host() == nullptr) return;
    std::vector<std::uint8_t> title(4096);
    for (std::size_t i = 0; i < title.size(); ++i) title[i] = static_cast<std::uint8_t>(i);
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    constexpr std::uintptr_t kSlot = 0x5150;
    shadow::register_buffer(kSlot, address, title.size());
    const auto copy = shadow::find(address, title.size());
    CHECK(copy.has_value());
    if (!copy) return;
    CHECK((*copy)->offset(address) == 0);
    CHECK((*copy)->update(address, title.size()) == title.size());
    CHECK(std::memcmp((*copy)->data(), title.data(), title.size()) == 0);
    title[0] = 200;
    CHECK((*copy)->update(address, title.size()) == title.size());
    CHECK((*copy)->data()[0] == 200);
    shadow::unregister_buffer(kSlot, address);
}

TEST(d3d12_xs_host_shadow_copies_back_only_what_shaders_changed) {
    if (test_host() == nullptr) return;
    std::vector<std::uint8_t> title(256, 4);
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    constexpr std::uintptr_t kSlot = 0x5151;
    shadow::register_buffer(kSlot, address, title.size());
    const auto copy = shadow::find(address, title.size());
    CHECK(copy.has_value());
    if (!copy) return;
    CHECK((*copy)->update(address, title.size()) == title.size());
    (*copy)->writing(address, 16);
    (*copy)->data()[0] = 9;
    CHECK((*copy)->copy_back(address, 16) == 16);
    CHECK(title[0] == 9);
    CHECK(title[1] == 4);
    shadow::unregister_buffer(kSlot, address);
}

TEST(d3d12_xs_host_watch_reports_written_pages_to_every_asker) {
    auto* memory = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, 4 * watch::kPageBytes, MEM_RESERVE | MEM_COMMIT | kWriteWatch, PAGE_READWRITE));
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const auto start = reinterpret_cast<std::size_t>(memory);
    const std::size_t end = start + 4 * watch::kPageBytes;
    const auto before = watch::writes(start, end, false);
    CHECK(before.has_value());
    memory[watch::kPageBytes + 8] = 1;
    const auto after = watch::writes(start, end, false);
    CHECK(after.has_value());
    if (after) {
        std::size_t reported = 0;
        for (const auto& [page, asking] : after->written) {
            if (asking != after->asked) continue;
            ++reported;
            CHECK(page == start / watch::kPageBytes + 1);
        }
        CHECK(reported == 1);
    }
    const auto later = watch::writes(start + watch::kPageBytes, start + 2 * watch::kPageBytes, false);
    CHECK(later.has_value());
    if (later && after) {
        CHECK(later->asked > after->asked);
        bool seen = false;
        for (const auto& [page, asking] : later->written)
            if (page == start / watch::kPageBytes + 1 && asking == after->asked) seen = true;
        CHECK(seen);
    }
    std::vector<std::uint8_t> plain(watch::kPageBytes, 0);
    CHECK(!watch::writes(reinterpret_cast<std::size_t>(plain.data()),
                         reinterpret_cast<std::size_t>(plain.data()) + watch::kPageBytes, true)
               .has_value());
    ::VirtualFree(memory, 0, MEM_RELEASE);
}

TEST(d3d12_xs_host_watch_opens_guarded_blocks_for_kernel_writes) {
    HANDLE section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          2 * static_cast<DWORD>(watch::kPageBytes), nullptr);
    CHECK(section != nullptr);
    if (section == nullptr) return;
    auto* view = static_cast<std::uint8_t*>(::MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 2 * watch::kPageBytes));
    CHECK(view != nullptr);
    if (view == nullptr) {
        ::CloseHandle(section);
        return;
    }
    const auto start = reinterpret_cast<std::size_t>(view);
    const auto guarded = watch::writes(start, start + 2 * watch::kPageBytes, true);
    CHECK(guarded.has_value());

    wchar_t folder[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    CHECK(::GetTempPathW(MAX_PATH, folder) > 0);
    CHECK(::GetTempFileNameW(folder, L"g6s", 0, path) > 0);
    HANDLE file = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        const char text[] = "from the disk";
        DWORD written = 0;
        CHECK(::WriteFile(file, text, 13, &written, nullptr) != 0);
        CHECK(written == 13);
        DWORD read = 0;
        CHECK(::ReadFile(file, view + 100, 13, &read, nullptr) == 0);
        watch::writing(start + 100, 13);
        CHECK(::SetFilePointer(file, 0, nullptr, FILE_BEGIN) == 0);
        CHECK(::ReadFile(file, view + 100, 13, &read, nullptr) != 0);
        CHECK(read == 13);
        CHECK(std::memcmp(view + 100, text, 13) == 0);
        const auto again = watch::writes(start, start + 2 * watch::kPageBytes, true);
        CHECK(again.has_value());
        if (again) {
            bool seen = false;
            for (const auto& [page, asking] : again->written)
                if (page == start / watch::kPageBytes && asking == again->asked) seen = true;
            CHECK(seen);
        }
        ::CloseHandle(file);
    }
    ::DeleteFileW(path);
    ::UnmapViewOfFile(view);
    watch::title_memory_remapped(start, 2 * watch::kPageBytes);
    ::CloseHandle(section);
}

TEST(d3d12_xs_host_textures_are_created_and_transitioned) {
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> memory(0x40000, 0);
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    resource::ResourceDesc desc = texture_desc();
    Object* texture_object = nullptr;
    CHECK(resource::create_at_address(nullptr, address, &desc, 0, nullptr, &resource::kResourceIids[0],
                                      &texture_object) == S_OK);
    ID3D12Resource* host_texture = texture::host_texture(texture_object);
    CHECK(host_texture != nullptr);
    CHECK(texture::host_texture_format(texture_object) == std::optional<std::uint32_t>(28));
    CHECK(texture::committed(memory.data(), memory.size()));
    CHECK(texture::title_written(host_texture));
    const auto changed = texture::host_transition(texture_object, 0x400);
    CHECK(changed.has_value());
    if (changed) CHECK(changed->second == 0);
    const auto back = texture::host_transition(texture_object, 0);
    CHECK(back.has_value());
    if (back) CHECK(back->second == 0x400);
    if (host_texture != nullptr) {
        auto list = gpu->new_list();
        CHECK(list != nullptr);
        if (list) {
            texture::upload(*list, texture_object, host_texture);
            list->close();
            gpu->execute({list.get()});
            CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
        }
    }
    CHECK(slot<std::uint32_t (*)(Object*)>(texture_object, 0x10)(texture_object) == 0);
}

TEST(d3d12_xs_host_page_mapper_is_resolved_from_xmem) {
    const auto exports = g6shim::xmem::private_api::resolve();
    CHECK(exports.map_alias != nullptr);
    if (exports.map_alias != nullptr) CHECK(exports.map_alias(0x1000, 0, 0) != 0);
}
