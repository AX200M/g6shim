// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fence.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <mutex>
#include <vector>

namespace g6shim::d3d12_xs::fence {

namespace {

const std::array<const void*, 24>& table();

struct Waiter {
    std::uint64_t value;
    void* event;
};

struct Fence : Object {
    Fence(std::uint64_t initial, std::uint32_t creation_flags)
        : Object(table().data()), value(initial), flags(creation_flags) {}

    std::atomic<std::uint64_t> value;
    std::uint32_t flags;
    std::mutex mutex;
    std::vector<Waiter> waiters;
};

std::array<const void*, 24> make_functions();

Fence* fence_of(Object* object) { return static_cast<Fence*>(object); }

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> table = make_functions();
    return table;
}

void set_event(void* event) {
    if (event != nullptr) ::SetEvent(static_cast<HANDLE>(event));
}

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kFence, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_fence(Object* object) { return release_boxed<Fence>(object); }

std::uint64_t completed(Object* object) {
    return fence_of(object)->value.load(std::memory_order_acquire);
}

HRESULT set_event_on_completion(Object* object, std::uint64_t value, void* event) {
    Fence* fence = fence_of(object);
    std::lock_guard<std::mutex> lock(fence->mutex);
    if (fence->value.load(std::memory_order_acquire) >= value) {
        set_event(event);
        return S_OK;
    }
    if (event == nullptr) {
        G6SHIM_WARN("d3d12_xs: blocking fence wait for %llu is not implemented",
                    static_cast<unsigned long long>(value));
        return E_NOTIMPL;
    }
    fence->waiters.push_back(Waiter{value, event});
    return S_OK;
}

HRESULT signal_impl(Object* object, std::uint64_t value) {
    Fence* fence = fence_of(object);
    std::lock_guard<std::mutex> lock(fence->mutex);
    fence->value.store(value, std::memory_order_release);
    auto& waiters = fence->waiters;
    for (auto it = waiters.begin(); it != waiters.end();) {
        if (it->value <= value) {
            set_event(it->event);
            it = waiters.erase(it);
        } else {
            ++it;
        }
    }
    return S_OK;
}

std::uint32_t creation_flags(Object* object) { return fence_of(object)->flags; }

std::uint64_t gpu_address(Object* object) {
    return reinterpret_cast<std::uint64_t>(&fence_of(object)->value);
}

std::array<const void*, 24> make_functions() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kFence, 24>());
    f[0] = reinterpret_cast<const void*>(&query);
    f[2] = reinterpret_cast<const void*>(&release_fence);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    f[8] = reinterpret_cast<const void*>(&completed);
    f[9] = reinterpret_cast<const void*>(&set_event_on_completion);
    f[10] = reinterpret_cast<const void*>(&signal_impl);
    f[11] = reinterpret_cast<const void*>(&creation_flags);
    f[12] = reinterpret_cast<const void*>(&gpu_address);
    return f;
}

}  // namespace

HRESULT create(Object*, std::uint64_t initial, std::uint32_t flags, const Iid* iid, Object** out) {
    G6SHIM_TRACE("d3d12_xs: fence with initial value %llu, flags %u", static_cast<unsigned long long>(initial),
                 flags);
    return hand_out(kFence, std::make_unique<Fence>(initial, flags), iid, kIids, out);
}

HRESULT signal(Object* object, std::uint64_t value) { return signal_impl(object, value); }

}  // namespace g6shim::d3d12_xs::fence
