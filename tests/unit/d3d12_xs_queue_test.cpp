// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/device.hpp"
#include "d3d12_xs/fence.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/list.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/queue.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

void use_newer_layout() {
    const std::uint32_t version = device::kCheckedVersion;
    Object* created = nullptr;
    CHECK(device::create(nullptr, &version, &device::kLevelIids[8], &created) == S_OK);
}

}  // namespace

TEST(d3d12_xs_queue_signals_complete_at_once) {
    use_newer_layout();
    const queue::Desc description{0, 0, 0, 0};
    Object* commands = nullptr;
    CHECK(queue::create(nullptr, &description, &queue::kIids[0], &commands) == S_OK);
    CHECK(commands != nullptr);
    if (commands == nullptr) return;
    std::uint32_t kind = 0xffff'ffffu;
    std::memcpy(&kind, reinterpret_cast<const std::uint8_t*>(commands) + 0x10, sizeof kind);
    CHECK(kind == 0);
    Object* fence_object = nullptr;
    CHECK(fence::create(nullptr, 0, 0, &fence::kIids[0], &fence_object) == S_OK);
    const auto signal = slot<HRESULT (*)(Object*, Object*, std::uint64_t)>(commands, 0x80);
    CHECK(signal(commands, fence_object, 7) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence_object, 0x40)(fence_object) == 7);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 0);
}

TEST(d3d12_xs_console_queues_are_created_as_on_pc) {
    use_newer_layout();
    const queue::DescX description{2, 0, 1, 0};
    Object* commands = nullptr;
    CHECK(queue::create_x(nullptr, &description, &queue::kIids[0], &commands) == S_OK);
    CHECK(commands != nullptr);
    if (commands == nullptr) return;
    queue::Desc read{};
    slot<void (*)(Object*, queue::Desc*)>(commands, 0xe0)(commands, &read);
    CHECK(read.kind == 2);
    CHECK(queue::create_x(nullptr, nullptr, &queue::kIids[0], &commands) == E_INVALIDARG);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 0);
}

TEST(d3d12_xs_newer_queues_have_their_methods_two_slots_later) {
    const auto& earlier = queue::functions(false);
    const auto& later = queue::functions(true);
    CHECK(earlier[10] == later[10]);
    CHECK(earlier[14] == later[16]);
    CHECK(earlier[30] == later[32]);
    CHECK(earlier[33] == later[35]);
    CHECK(earlier[16] != later[16]);
}

TEST(d3d12_xs_timestamps_and_values_are_written) {
    std::array<std::uint64_t, 2> at{};
    host::Written::of(0x1'0000'0002ull).write(reinterpret_cast<std::uint8_t*>(at.data()), 4);
    CHECK(at[0] == 2);
    host::Written::gpu_timestamp().write(reinterpret_cast<std::uint8_t*>(at.data() + 1), 8);
    const std::uint64_t first = at[1];
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    host::Written::gpu_timestamp().write(reinterpret_cast<std::uint8_t*>(at.data() + 1), 8);
    CHECK(at[1] > first);
}

TEST(d3d12_xs_submitted_lists_run_on_the_host) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    if (host::host() == nullptr) return;
    use_newer_layout();
    Object* allocator = nullptr;
    CHECK(list::create_allocator(nullptr, 0, &list::kAllocatorIids[0], &allocator) == S_OK);
    Object* commands = nullptr;
    CHECK(list::create_list(nullptr, 0, 0, allocator, nullptr, &list::kListIids[1], &commands) == S_OK);
    const queue::Desc description{0, 0, 0, 0};
    Object* queue_object = nullptr;
    CHECK(queue::create(nullptr, &description, &queue::kIids[0], &queue_object) == S_OK);
    CHECK(slot<HRESULT (*)(Object*)>(commands, 0x48)(commands) == S_OK);
    Object* submitted[1] = {commands};
    slot<void (*)(Object*, std::uint32_t, Object* const*)>(queue_object, 0x50)(queue_object, 1, submitted);
    Object* fence_object = nullptr;
    CHECK(fence::create(nullptr, 0, 0, &fence::kIids[0], &fence_object) == S_OK);
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(event != nullptr);
    CHECK(slot<HRESULT (*)(Object*, std::uint64_t, void*)>(fence_object, 0x48)(fence_object, 42, event) == S_OK);
    CHECK(slot<HRESULT (*)(Object*, Object*, std::uint64_t)>(queue_object, 0x80)(queue_object, fence_object, 42) ==
          S_OK);
    CHECK(::WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence_object, 0x40)(fence_object) == 42);
    ::CloseHandle(event);
    CHECK(slot<std::uint32_t (*)(Object*)>(queue_object, 0x10)(queue_object) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(allocator, 0x10)(allocator) == 0);
}

void write_plane(std::vector<std::uint8_t>& bytes, std::uint32_t index, std::uint64_t token, std::uint32_t count,
                 Object* const* resources) {
    const std::size_t at = static_cast<std::size_t>(index) * queue::kPlaneRecordBytes;
    std::memcpy(bytes.data() + at + queue::kPlaneTokenOffset, &token, sizeof token);
    std::memcpy(bytes.data() + at + queue::kPlaneCountOffset, &count, sizeof count);
    std::memcpy(bytes.data() + at + queue::kPlaneResourcesOffset, &resources, sizeof resources);
}

TEST(d3d12_xs_present_plane_records_are_read_at_their_stride) {
    constexpr std::uint32_t kRecords = 8;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kRecords) * queue::kPlaneRecordBytes, 0xCD);
    Object* resource1 = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x1000));
    Object* resource3 = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x3000));
    Object* resource6 = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x6000));
    Object* resource7 = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x7000));
    Object* resource8 = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x8000));
    Object* missing = nullptr;
    Object* list1[] = {resource1};
    Object* list3[] = {resource3};
    Object* list_null[] = {missing};
    Object* list6[] = {resource6};
    Object* list7[] = {resource7};
    Object* list8[] = {resource8};
    write_plane(bytes, 0, 1, 1, list1);
    write_plane(bytes, 1, 2, 1, nullptr);
    write_plane(bytes, 2, 3, 1, list3);
    write_plane(bytes, 3, 4, 0, list3);
    write_plane(bytes, 4, 5, 1, list_null);
    write_plane(bytes, 5, 6, 1, list6);
    write_plane(bytes, 6, 7, 1, list7);
    write_plane(bytes, 7, 8, 1, list8);

    std::array<queue::PlaneRecord, 8> out{};
    CHECK(queue::read_plane_records(bytes.data(), 3, out.data(), 8) == 2);
    CHECK(out[0].token == 1);
    CHECK(out[0].resources == list1);
    CHECK(out[0].resources[0] == resource1);
    CHECK(out[1].token == 3);
    CHECK(out[1].resources[0] == resource3);

    CHECK(queue::read_plane_records(bytes.data(), kRecords, out.data(), 8) == queue::kMaxPresentedPlanes);
    CHECK(out[0].token == 1);
    CHECK(out[1].token == 3);
    CHECK(out[2].token == 6);
    CHECK(out[2].resources[0] == resource6);
    CHECK(out[3].token == 7);
    CHECK(out[3].resources[0] == resource7);
    CHECK(queue::read_plane_records(nullptr, kRecords, out.data(), 8) == 0);
    CHECK(queue::read_plane_records(bytes.data(), 0, out.data(), 8) == 0);
}
