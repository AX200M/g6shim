// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "queue.hpp"

#include "device.hpp"
#include "draw.hpp"
#include "fence.hpp"
#include "host.hpp"
#include "list.hpp"
#include "resource.hpp"
#include "texture.hpp"

#include <g6shim/log.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::queue {

namespace {

constexpr std::uint64_t kPageBytes = 64 << 10;

struct Queue : Object {
    Queue(const void* const* table, std::uint32_t list_type, const Desc& description)
        : Object(table), kind(list_type), desc(description) {}

    std::uint32_t kind;
    Desc desc;
};

std::atomic<std::uint64_t> g_frames{0};

void query_interface(Object* object, const Iid* iid, Object** out) {
    answer(kQueue, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_queue(Object* object) { return release_boxed<Queue>(object); }

void execute(Object* queue_object, std::uint32_t count, Object* const* lists) {
    if (lists == nullptr) return;
    if (host::Host* gpu = host::host()) {
        std::vector<host::HostList*> runs;
        runs.reserve(count);
        for (const Object* title : std::span<Object* const>(lists, count)) {
            if (host::HostList* host_list = list::host_list(const_cast<Object*>(title)))
                runs.push_back(host_list);
        }
        if (runs.empty()) return;
        host::await_queue_waits(queue_object);
        host::begin_queue_submission(queue_object);
        gpu->execute(runs);
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        G6SHIM_WARN("d3d12_xs: command lists submitted; no host GPU runs them");
    });
}

void execute_x(Object* object, std::uint32_t count, Object* const* lists, std::uint64_t, std::uint32_t) {
    execute(object, count, lists);
}

void marker(Object*) {}

HRESULT signal(Object* queue_object, Object* fence_object, std::uint64_t value) {
    if (fence_object == nullptr) return E_INVALIDARG;
    host::note_queue_signal(queue_object, fence_object, value);
    return S_OK;
}

HRESULT signal2_x(Object* object, Object* fence_object, std::uint64_t value, const void*, const void*) {
    return signal(object, fence_object, value);
}

HRESULT wait(Object* queue_object, Object* fence_object, std::uint64_t value) {
    if (fence_object == nullptr) return E_INVALIDARG;
    host::note_queue_wait(queue_object, fence_object, value);
    return S_OK;
}

HRESULT timestamp_frequency(Object*, std::uint64_t* out) {
    if (out == nullptr) return E_INVALIDARG;
    *out = host::kTimestampFrequency;
    return S_OK;
}

HRESULT present(Object*, std::uint32_t planes, const Plane* plane_parameters, const void*) {
    const std::uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((frame & (frame - 1)) == 0) {
        const auto [draws, dispatches] = list::take_counts();
        G6SHIM_INFO("d3d12_xs: frame %llu presented (draws %llu, drawn %llu, dispatches %llu, dispatched %llu)",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(draws),
                    static_cast<unsigned long long>(draw::drawn()), static_cast<unsigned long long>(dispatches),
                    static_cast<unsigned long long>(draw::dispatched()));
    }
    if (planes == 0 || plane_parameters == nullptr) return S_OK;
    std::array<PlaneRecord, kMaxPresentedPlanes> records{};
    const std::uint32_t found = read_plane_records(plane_parameters, planes, records.data(), kMaxPresentedPlanes);
    host::Host* gpu = host::host();
    if (gpu == nullptr || found == 0) return S_OK;
    constexpr std::uint32_t kCopySource = 0x800;
    constexpr std::uint32_t kPixelShaderResource = 0x80;
    std::array<host::PresentedPlane, kMaxPresentedPlanes> host_planes{};
    std::array<Object*, kMaxPresentedPlanes> resources{};
    std::uint32_t count = 0;
    for (std::uint32_t index = 0; index < found; ++index) {
        Object* resource = records[index].resources[0];
        ID3D12Resource* texture = texture::host_texture(resource);
        if (texture == nullptr) continue;
        resources[count] = resource;
        host_planes[count].texture = texture;
        ++count;
    }
    if (count == 0) return S_OK;
    const std::uint32_t wanted = count == 1 ? kCopySource : kPixelShaderResource;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t state = wanted;
        if (const auto changed = resource::host_transition(resources[index], wanted)) state = changed->second;
        host_planes[index].state = state;
    }
    if (count == 1)
        gpu->present(host_planes[0].texture, host_planes[0].state, frame);
    else
        gpu->present({host_planes.data(), count}, frame);
    for (std::uint32_t index = 0; index < count; ++index)
        resource::host_transition(resources[index], host_planes[index].state);
    return S_OK;
}

HRESULT copy_page_mappings(Object*, std::uint32_t count, const PageBatch* batches, std::uint64_t pool,
                           std::uint32_t pool_pages, std::uint32_t) {
    const auto mapper = resource::page_mapper();
    if (!mapper) return E_INVALIDARG;
    if (batches == nullptr && count > 0) return E_INVALIDARG;
    bool ok = true;
    if (count > 0) {
        for (const PageBatch& batch : std::span<const PageBatch>(batches, count)) {
            if (batch.ranges == nullptr) continue;
            std::uint64_t at = batch.destination;
            for (const PageRange& range : std::span<const PageRange>(batch.ranges, batch.count)) {
                const std::uint64_t length = static_cast<std::uint64_t>(range.count) * kPageBytes;
                const auto inside = [pool_pages, &range](std::uint32_t pages) {
                    return range.start <= pool_pages && pages <= pool_pages - range.start;
                };
                const std::uint64_t source = pool + static_cast<std::uint64_t>(range.start) * kPageBytes;
                bool mapped = false;
                switch (range.kind) {
                case 0:
                    if (inside(range.count)) mapped = (*mapper)(at, source, length);
                    break;
                case 1:
                    if (inside(1)) {
                        mapped = true;
                        for (std::uint64_t page = 0; page < range.count; ++page)
                            mapped = (*mapper)(at + page * kPageBytes, source, kPageBytes) && mapped;
                    }
                    break;
                case 2: mapped = (*mapper)(at, 0, length); break;
                case 3: mapped = true; break;
                default: break;
                }
                ok = ok && mapped;
                at += length;
            }
        }
    }
    if (!ok) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a page mapping failed");
        });
    }
    return S_OK;
}

void description(Object* object, Desc* out) {
    if (out != nullptr) *out = static_cast<Queue*>(object)->desc;
}

std::size_t newer_slot(std::size_t slot) { return slot >= 14 ? slot + 2 : slot; }

std::array<const void*, 36> make_table(bool newer) {
    std::array<const void*, 36> functions = d3d12_object(missing_table<kQueue, 36>());
    const std::array<std::pair<std::size_t, const void*>, 15> methods{{
        {0, reinterpret_cast<const void*>(&query_interface)},
        {2, reinterpret_cast<const void*>(&release_queue)},
        {7, reinterpret_cast<const void*>(&parent_device)},
        {10, reinterpret_cast<const void*>(&execute)},
        {11, reinterpret_cast<const void*>(&marker)},
        {12, reinterpret_cast<const void*>(&marker)},
        {13, reinterpret_cast<const void*>(&marker)},
        {14, reinterpret_cast<const void*>(&signal)},
        {15, reinterpret_cast<const void*>(&wait)},
        {16, reinterpret_cast<const void*>(&timestamp_frequency)},
        {26, reinterpret_cast<const void*>(&description)},
        {29, reinterpret_cast<const void*>(&copy_page_mappings)},
        {30, reinterpret_cast<const void*>(&present)},
        {32, reinterpret_cast<const void*>(&signal2_x)},
        {33, reinterpret_cast<const void*>(&execute_x)},
    }};
    for (const auto& [slot, method] : methods) functions[newer ? newer_slot(slot) : slot] = method;
    return functions;
}

}  // namespace
std::uint32_t read_plane_records(const void* parameters, std::uint32_t planes, PlaneRecord* out,
                                 std::uint32_t capacity) {
    if (parameters == nullptr || out == nullptr || planes == 0 || capacity == 0) return 0;
    const auto* bytes = static_cast<const std::uint8_t*>(parameters);
    const std::uint32_t accept = capacity < kMaxPresentedPlanes ? capacity : kMaxPresentedPlanes;
    std::uint32_t written = 0;
    for (std::uint32_t index = 0; index < planes && written < accept; ++index) {
        const auto* record = bytes + static_cast<std::size_t>(index) * kPlaneRecordBytes;
        PlaneRecord plane{};
        std::memcpy(&plane.token, record + kPlaneTokenOffset, sizeof plane.token);
        std::memcpy(&plane.count, record + kPlaneCountOffset, sizeof plane.count);
        std::memcpy(&plane.resources, record + kPlaneResourcesOffset, sizeof plane.resources);
        if (plane.count == 0 || plane.resources == nullptr || plane.resources[0] == nullptr) continue;
        out[written++] = plane;
    }
    return written;
}

const std::array<const void*, 36>& functions(bool newer) {
    static const std::array<const void*, 36> earlier = make_table(false);
    static const std::array<const void*, 36> later = make_table(true);
    return newer ? later : earlier;
}

HRESULT create(Object*, const Desc* desc, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    auto queue_object = std::make_unique<Queue>(functions(device::newer()).data(), desc->kind, *desc);
    return hand_out(kQueue, std::move(queue_object), iid, kIids, out);
}

HRESULT create_x(Object* device_object, const DescX* desc, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    const Desc plain{desc->kind, 0, desc->flags, 1};
    return create(device_object, &plain, iid, out);
}

}  // namespace g6shim::d3d12_xs::queue
