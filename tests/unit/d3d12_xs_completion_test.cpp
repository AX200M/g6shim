// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/fence.hpp"
#include "d3d12_xs/host.hpp"
#include "d3d12_xs/queue.hpp"
#include "d3d12_xs/shadow.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace g6shim::d3d12_xs;

void g6shim_completion_tests_anchor() {}

namespace {

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

struct CompletionGuard {
    ~CompletionGuard() {
        host::pause_completion_for_test(false);
        host::set_device_removed_for_test(false);
        ::SetEnvironmentVariableA("G6SHIM_COMPLETION_THREAD", nullptr);
        ::SetEnvironmentVariableA("G6SHIM_FRAME_TIMING", nullptr);
        if (host::host() != nullptr) host::wait_idle();
    }
};

host::Host* test_host() {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    return host::host();
}

bool gpu_store(host::Host& gpu, host::HostList& list, const std::shared_ptr<shadow::Shadow>& shadow,
               std::uint64_t address, const std::uint8_t* bytes, std::size_t count) {
    auto upload = gpu.upload_buffer(count);
    if (!upload) return false;
    std::memcpy(upload->second, bytes, count);
    list.keep(upload->first.Get());
    const std::uint64_t offset = address - shadow->start();
    D3D12_RESOURCE_BARRIER to_copy =
        host::transition(shadow->buffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list.list()->ResourceBarrier(1, &to_copy);
    list.list()->CopyBufferRegion(shadow->buffer(), offset, upload->first.Get(), 0, count);
    D3D12_RESOURCE_BARRIER to_common =
        host::transition(shadow->buffer(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list.list()->ResourceBarrier(1, &to_common);
    list.defer_gpu_write(shadow, static_cast<std::size_t>(address), count);
    return true;
}

std::shared_ptr<shadow::Shadow> registered(std::vector<std::uint8_t>& title, std::uintptr_t slot) {
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    shadow::register_buffer(slot, address, title.size());
    const auto copy = shadow::find(address, title.size());
    if (!copy) return nullptr;
    if ((*copy)->update(address, title.size()) != title.size()) return nullptr;
    return *copy;
}

Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&buffer))))
        return nullptr;
    return buffer;
}

void print_interval(const char* label, double wall_ms, const host::IntervalTiming& timing) {
    std::printf(
        "%s wall %.2f ms, submissions %llu, prepare %.2f ms, execute %.2f ms, fence %.2f ms, copy-back %.2f ms "
        "%llu bytes, gpu %.2f ms (%llu samples)\n",
        label, wall_ms, static_cast<unsigned long long>(timing.submissions), timing.prepare.milliseconds,
        timing.execute_call.milliseconds, timing.wait.milliseconds, timing.copy_back.milliseconds,
        static_cast<unsigned long long>(timing.copy_back.bytes), timing.gpu.milliseconds,
        static_cast<unsigned long long>(timing.gpu_samples));
}

}  // namespace

TEST(d3d12_xs_completion_cpu_reader_sees_gpu_bytes_after_the_fence) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(64, 1);
    const auto shadow = registered(title, 0x7101);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    const std::uint8_t stored[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
    list->close();
    const queue::Desc description{0, 0, 0, 0};
    Object* queue_object = nullptr;
    CHECK(queue::create(nullptr, &description, &queue::kIids[0], &queue_object) == S_OK);
    Object* fence_object = nullptr;
    CHECK(fence::create(nullptr, 0, 0, &fence::kIids[0], &fence_object) == S_OK);
    host::pause_completion_for_test(true);
    host::begin_queue_submission(queue_object);
    gpu->execute({list.get()});
    host::note_queue_signal(queue_object, fence_object, 4);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence_object, 0x40)(fence_object) == 0);
    CHECK(title[0] == 1);
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(slot<HRESULT (*)(Object*, std::uint64_t, void*)>(fence_object, 0x48)(fence_object, 4, event) == S_OK);
    host::pause_completion_for_test(false);
    CHECK(::WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence_object, 0x40)(fence_object) == 4);
    CHECK(title[0] == 9);
    CHECK(title[15] == 9);
    CHECK(title[16] == 1);
    ::CloseHandle(event);
    shadow::unregister_buffer(0x7101, address);
    CHECK(slot<std::uint32_t (*)(Object*)>(fence_object, 0x10)(fence_object) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(queue_object, 0x10)(queue_object) == 0);
}

TEST(d3d12_xs_completion_gpu_reader_does_not_refresh_stale_bytes) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(64, 1);
    const auto shadow = registered(title, 0x7102);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto writer = gpu->new_list();
    CHECK(writer != nullptr);
    if (!writer) return;
    const std::uint8_t stored[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    CHECK(gpu_store(*gpu, *writer, shadow, address, stored, sizeof stored));
    writer->close();
    host::pause_completion_for_test(true);
    gpu->execute({writer.get()});
    auto reader = gpu->new_list();
    CHECK(reader != nullptr);
    if (!reader) return;
    reader->defer_refresh(shadow, static_cast<std::size_t>(address), sizeof stored);
    Microsoft::WRL::ComPtr<ID3D12Resource> readback = readback_buffer(gpu->device(), sizeof stored);
    CHECK(readback != nullptr);
    if (!readback) return;
    reader->keep(readback.Get());
    D3D12_RESOURCE_BARRIER to_source =
        host::transition(shadow->buffer(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    reader->list()->ResourceBarrier(1, &to_source);
    reader->list()->CopyBufferRegion(readback.Get(), 0, shadow->buffer(), 0, sizeof stored);
    D3D12_RESOURCE_BARRIER to_common =
        host::transition(shadow->buffer(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    reader->list()->ResourceBarrier(1, &to_common);
    reader->close();
    std::atomic<bool> done{false};
    std::thread second([&] {
        gpu->execute({reader.get()});
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!done.load(std::memory_order_acquire));
    host::pause_completion_for_test(false);
    second.join();
    host::wait_idle();
    void* mapped = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    if (mapped != nullptr) {
        CHECK(static_cast<const std::uint8_t*>(mapped)[0] == 7);
        readback->Unmap(0, nullptr);
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    shadow::unregister_buffer(0x7102, address);
}

TEST(d3d12_xs_completion_newer_cpu_write_wins) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(64, 1);
    const auto shadow = registered(title, 0x7103);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    const std::uint8_t stored[32] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                     2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
    list->close();
    host::pause_completion_for_test(true);
    gpu->execute({list.get()});
    title[0] = 3;
    std::memset(title.data() + 16, 5, 16);
    host::pause_completion_for_test(false);
    host::wait_idle();
    CHECK(title[0] == 3);
    CHECK(shadow->data()[0] == 3);
    CHECK(title[1] == 2);
    CHECK(title[16] == 5);
    CHECK(shadow->data()[16] == 5);
    shadow::unregister_buffer(0x7103, address);
}

TEST(d3d12_xs_completion_reuses_lists_while_work_is_in_flight) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(32, 1);
    const auto shadow = registered(title, 0x7104);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    host::pause_completion_for_test(true);
    for (std::uint8_t i = 0; i < 3; ++i) {
        if (i != 0) list->reset();
        const std::uint8_t stored[4] = {i, i, i, i};
        CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
        list->close();
        gpu->execute({list.get()});
        CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    }
    CHECK(host::in_flight_for_test() == 3);
    std::atomic<bool> done{false};
    std::thread fourth([&] {
        list->reset();
        const std::uint8_t stored[4] = {8, 8, 8, 8};
        CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
        list->close();
        gpu->execute({list.get()});
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!done.load(std::memory_order_acquire));
    host::pause_completion_for_test(false);
    fourth.join();
    host::wait_idle();
    CHECK(title[0] == 8);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    shadow::unregister_buffer(0x7104, address);
}

TEST(d3d12_xs_completion_abandons_in_flight_work_without_hanging) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(32, 1);
    const auto shadow = registered(title, 0x7105);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    const std::uint8_t stored[4] = {6, 6, 6, 6};
    CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
    list->close();
    host::pause_completion_for_test(true);
    gpu->execute({list.get()});
    host::set_device_removed_for_test(true);
    host::abandon_in_flight_for_test();
    CHECK(host::in_flight_for_test() == 0);
    CHECK(title[0] == 1);
    const auto started = std::chrono::steady_clock::now();
    host::wait_for_writes(title.data(), title.size());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    host::set_device_removed_for_test(false);
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    shadow::unregister_buffer(0x7105, address);
}

TEST(d3d12_xs_completion_queue_wait_orders_a_later_queue) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(32, 1);
    const auto shadow = registered(title, 0x7106);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    const queue::Desc description{0, 0, 0, 0};
    Object* queue_a = nullptr;
    Object* queue_b = nullptr;
    CHECK(queue::create(nullptr, &description, &queue::kIids[0], &queue_a) == S_OK);
    CHECK(queue::create(nullptr, &description, &queue::kIids[0], &queue_b) == S_OK);
    Object* fence_object = nullptr;
    CHECK(fence::create(nullptr, 0, 0, &fence::kIids[0], &fence_object) == S_OK);
    auto writer = gpu->new_list();
    CHECK(writer != nullptr);
    if (!writer) return;
    const std::uint8_t stored[4] = {4, 4, 4, 4};
    CHECK(gpu_store(*gpu, *writer, shadow, address, stored, sizeof stored));
    writer->close();
    host::pause_completion_for_test(true);
    host::begin_queue_submission(queue_a);
    gpu->execute({writer.get()});
    host::note_queue_signal(queue_a, fence_object, 1);
    host::note_queue_wait(queue_b, fence_object, 1);
    std::atomic<bool> blocked{false};
    std::thread later([&] {
        blocked.store(true, std::memory_order_release);
        host::await_queue_waits(queue_b);
        blocked.store(false, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(blocked.load(std::memory_order_acquire));
    host::pause_completion_for_test(false);
    later.join();
    host::wait_idle();
    CHECK(title[0] == 4);
    CHECK(slot<std::uint64_t (*)(Object*)>(fence_object, 0x40)(fence_object) == 1);
    shadow::unregister_buffer(0x7106, address);
    CHECK(slot<std::uint32_t (*)(Object*)>(queue_a, 0x10)(queue_a) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(queue_b, 0x10)(queue_b) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(fence_object, 0x10)(fence_object) == 0);
}

TEST(d3d12_xs_completion_reports_phase_times) {
    CompletionGuard guard;
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    std::vector<std::uint8_t> title(32, 1);
    const auto shadow = registered(title, 0x7107);
    CHECK(shadow != nullptr);
    if (!shadow) return;
    const auto address = reinterpret_cast<std::uint64_t>(title.data());
    auto run = [&](const char* label) {
        host::take_submission_interval();
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < 8; ++i) {
            auto list = gpu->new_list();
            if (!list) return;
            const std::uint8_t stored[4] = {static_cast<std::uint8_t>(i + 1), 0, 0, 0};
            CHECK(gpu_store(*gpu, *list, shadow, address, stored, sizeof stored));
            list->close();
            gpu->execute({list.get()});
        }
        host::wait_idle();
        const double wall =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        print_interval(label, wall, host::take_submission_interval());
    };
    ::SetEnvironmentVariableA("G6SHIM_COMPLETION_THREAD", "0");
    ::SetEnvironmentVariableA("G6SHIM_FRAME_TIMING", "1");
    run("sync");
    ::SetEnvironmentVariableA("G6SHIM_COMPLETION_THREAD", "1");
    run("async");
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    shadow::unregister_buffer(0x7107, address);
}

TEST(d3d12_xs_completion_sync_path_still_publishes_before_return) {
    CompletionGuard guard;
    ::SetEnvironmentVariableA("G6SHIM_COMPLETION_THREAD", "0");
    host::Host* gpu = test_host();
    if (gpu == nullptr) return;
    host::wait_idle();
    auto list = gpu->new_list();
    CHECK(list != nullptr);
    if (!list) return;
    std::uint64_t destination = 0;
    list->defer_write(reinterpret_cast<std::uint8_t*>(&destination), host::Written::of(0x21), 8);
    list->close();
    gpu->execute({list.get()});
    CHECK(destination == 0x21);
}
