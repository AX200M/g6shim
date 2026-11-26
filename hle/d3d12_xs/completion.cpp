// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.hpp"

#include "descriptor_host.hpp"
#include "fence.hpp"
#include "shadow.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::host {

namespace {

constexpr DWORD kWaitSliceMs = 50;

struct Timing {
    std::atomic<std::uint64_t> prepare_ns{0};
    std::atomic<std::uint64_t> execute_ns{0};
    std::atomic<std::uint64_t> fence_wait_ns{0};
    std::atomic<std::uint64_t> copy_back_ns{0};
    std::atomic<std::uint64_t> gpu_busy_ns{0};
    std::atomic<std::uint64_t> submissions{0};
};

Timing g_timing;

struct Flight {
    std::uint64_t host_value = 0;
    std::vector<std::tuple<std::shared_ptr<shadow::Shadow>, std::size_t, std::size_t>> writes;
    std::vector<WriteStep> deferred;
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> kept;
    card::Batch cards;
    bool cards_open = false;
    SubmissionTiming timing;
};

struct PendingSignal {
    std::uint64_t after_host = 0;
    Object* fence = nullptr;
    std::uint64_t value = 0;
};

struct QueueTrack {
    std::uint64_t last_flight = 0;
    std::vector<std::pair<Object*, std::uint64_t>> waits;
};


struct Lane {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Flight> flights;
    std::vector<PendingSignal> signals;
    std::unordered_map<Object*, QueueTrack> queues;
    std::thread thread;
    HANDLE fence_event = nullptr;
    HANDLE wake = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> retiring{false};
    std::atomic<bool> abandon{false};
    std::atomic<bool> removed{false};
    std::uint64_t landed = 0;

    ~Lane() {
        stop.store(true, std::memory_order_release);
        paused.store(false, std::memory_order_release);
        cv.notify_all();
        if (wake != nullptr) ::SetEvent(wake);
        if (thread.joinable()) thread.join();
        if (fence_event != nullptr) ::CloseHandle(fence_event);
        if (wake != nullptr) ::CloseHandle(wake);
        fence_event = nullptr;
        wake = nullptr;
    }
};

Lane& lane() {
    static Lane value;
    return value;
}

thread_local Object* t_queue = nullptr;

bool overlaps(std::size_t left, std::size_t left_bytes, std::size_t right, std::size_t right_bytes) {
    if (left_bytes == 0 || right_bytes == 0) return false;
    if (left > std::numeric_limits<std::size_t>::max() - left_bytes) return false;
    if (right > std::numeric_limits<std::size_t>::max() - right_bytes) return false;
    return left < right + right_bytes && right < left + left_bytes;
}

std::uint64_t fence_value(Object* fence) {
    if (fence == nullptr) return 0;
    return slot<std::uint64_t (*)(Object*)>(fence, 0x40)(fence);
}

void discard_writes(const std::vector<std::tuple<std::shared_ptr<shadow::Shadow>, std::size_t, std::size_t>>& writes) {
    for (const auto& [shadow, address, bytes] : writes)
        if (shadow) shadow->discard_write(address, bytes);
}

void publish(const std::vector<PendingSignal>& ready) {
    for (const PendingSignal& signal : ready)
        if (signal.fence != nullptr) fence::signal(signal.fence, signal.value);
}

std::vector<PendingSignal> take_ready(Lane& owned) {
    std::vector<PendingSignal> ready;
    auto it = owned.signals.begin();
    while (it != owned.signals.end()) {
        if (it->after_host <= owned.landed) {
            ready.push_back(*it);
            it = owned.signals.erase(it);
        } else {
            ++it;
        }
    }
    return ready;
}

bool device_removed() {
    if (lane().removed.load(std::memory_order_acquire)) return true;
    Host* gpu = host();
    return gpu != nullptr && gpu->device() != nullptr && FAILED(gpu->device()->GetDeviceRemovedReason());
}


bool wait_host_fence(std::uint64_t value) {
    Host* gpu = host();
    Lane& owned = lane();
    if (gpu == nullptr || gpu->fence() == nullptr || owned.fence_event == nullptr) return false;
    if (gpu->fence()->GetCompletedValue() >= value) return !device_removed();
    const auto started = std::chrono::steady_clock::now();
    const HANDLE handles[2] = {owned.fence_event, owned.wake};
    for (;;) {
        if (owned.stop.load(std::memory_order_acquire) || owned.abandon.load(std::memory_order_acquire) ||
            device_removed())
            return false;
        if (FAILED(gpu->fence()->SetEventOnCompletion(value, owned.fence_event))) return false;
        const DWORD waited = ::WaitForMultipleObjects(2, handles, FALSE, kWaitSliceMs);
        if (gpu->fence()->GetCompletedValue() >= value) {
            g_timing.fence_wait_ns.fetch_add(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - started)
                                                .count()),
                std::memory_order_relaxed);
            return !device_removed();
        }
        if (waited == WAIT_OBJECT_0 + 1) return false;
        if (owned.stop.load(std::memory_order_acquire) || owned.abandon.load(std::memory_order_acquire) ||
            device_removed())
            return false;
    }
}


void release_cards(Flight& flight) {
    if (!flight.cards_open) return;
    flight.cards_open = false;
    card::release_recorder();
}

void land_cards(Flight& flight) {
    if (!flight.cards_open) return;
    Host* gpu = host();
    if (gpu == nullptr || gpu->device() == nullptr || gpu->queue() == nullptr) return;
    flight.cards.read_changed(gpu->device(), gpu->queue());
}

void sync_cards(Flight& flight) {
    if (!flight.cards_open) return;
    Host* gpu = host();
    if (gpu != nullptr && gpu->device() != nullptr && gpu->queue() != nullptr)
        flight.cards.sync_kept(gpu->device(), gpu->queue());
    release_cards(flight);
}

std::uint64_t copy_back_flight(Flight& flight) {
    if (flight.cards_open) land_cards(flight);
    std::uint64_t copied = 0;
    for (const auto& [shadow, address, bytes] : flight.writes)
        if (shadow) copied += shadow->copy_back(address, bytes);
    if (flight.cards_open) sync_cards(flight);
    for (const WriteStep& write : flight.deferred) write.written.write(write.destination, write.bytes);
    shadow::submission_completed();
    descriptor_host::release_retired_views();
    return copied;
}

void retire_one() {
    Lane& owned = lane();
    std::uint64_t value = 0;
    {
        std::lock_guard<std::mutex> lock(owned.mutex);
        if (owned.flights.empty()) return;
        value = owned.flights.front().host_value;
        owned.retiring.store(true, std::memory_order_release);
    }
    const auto waited = std::chrono::steady_clock::now();
    const bool confirmed = wait_host_fence(value);
    Flight snapshot;
    {
        std::lock_guard<std::mutex> lock(owned.mutex);
        if (owned.flights.empty() || owned.flights.front().host_value != value) {
            owned.retiring.store(false, std::memory_order_release);
            owned.cv.notify_all();
            return;
        }
        snapshot.host_value = value;
        snapshot.writes = std::move(owned.flights.front().writes);
        snapshot.deferred = std::move(owned.flights.front().deferred);
        snapshot.cards = std::move(owned.flights.front().cards);
        snapshot.cards_open = owned.flights.front().cards_open;
        owned.flights.front().cards_open = false;
        snapshot.timing = owned.flights.front().timing;
    }
    snapshot.timing.wait.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count();
    if (confirmed) {
        const auto copying = std::chrono::steady_clock::now();
        snapshot.timing.copy_back.bytes = copy_back_flight(snapshot);
        snapshot.timing.copy_back.milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copying).count();
    } else {
        discard_writes(snapshot.writes);
        release_cards(snapshot);
    }
    record_submission_timing(snapshot.timing);
    std::vector<PendingSignal> ready;
    {
        std::lock_guard<std::mutex> lock(owned.mutex);
        if (!owned.flights.empty() && owned.flights.front().host_value == value) owned.flights.pop_front();
        if (confirmed) owned.landed = std::max(owned.landed, value);
        ready = take_ready(owned);
        owned.retiring.store(false, std::memory_order_release);
    }
    if (confirmed) publish(ready);
    owned.cv.notify_all();
}

void loop() {
    try {
        Lane& owned = lane();
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(owned.mutex);
                owned.cv.wait(lock, [&] {
                    return owned.stop.load(std::memory_order_acquire) ||
                           owned.abandon.load(std::memory_order_acquire) ||
                           (!owned.paused.load(std::memory_order_acquire) && !owned.flights.empty());
                });
                if (owned.abandon.load(std::memory_order_acquire)) {
                    for (Flight& flight : owned.flights) {
                        discard_writes(flight.writes);
                        release_cards(flight);
                    }
                    owned.flights.clear();
                    owned.signals.clear();
                    owned.abandon.store(false, std::memory_order_release);
                    owned.retiring.store(false, std::memory_order_release);
                    lock.unlock();
                    owned.cv.notify_all();
                    continue;
                }
                if (owned.stop.load(std::memory_order_acquire) && owned.flights.empty()) return;
                if (owned.paused.load(std::memory_order_acquire) || owned.flights.empty()) continue;
            }
            retire_one();
            if (device_removed()) {
                std::lock_guard<std::mutex> lock(owned.mutex);
                for (Flight& flight : owned.flights) {
                    discard_writes(flight.writes);
                    release_cards(flight);
                }
                owned.flights.clear();
                owned.cv.notify_all();
            }
        }
    } catch (const std::exception& error) {
        G6SHIM_ERROR("d3d12_xs: completion thread stopped: %s", error.what());
        Lane& owned = lane();
        std::lock_guard<std::mutex> lock(owned.mutex);
        for (Flight& flight : owned.flights) {
            discard_writes(flight.writes);
            release_cards(flight);
        }
        owned.flights.clear();
        owned.abandon.store(true, std::memory_order_release);
        owned.retiring.store(false, std::memory_order_release);
        owned.cv.notify_all();
    } catch (...) {
        G6SHIM_ERROR("d3d12_xs: completion thread stopped");
        Lane& owned = lane();
        std::lock_guard<std::mutex> lock(owned.mutex);
        for (Flight& flight : owned.flights) {
            discard_writes(flight.writes);
            release_cards(flight);
        }
        owned.flights.clear();
        owned.abandon.store(true, std::memory_order_release);
        owned.retiring.store(false, std::memory_order_release);
        owned.cv.notify_all();
    }
}

void ensure_started() {
    Lane& owned = lane();
    if (owned.started.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(owned.mutex);
    if (owned.started.load(std::memory_order_relaxed)) return;
    owned.fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    owned.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    owned.started.store(true, std::memory_order_release);
    owned.thread = std::thread(loop);
}


}  // namespace

bool completion_thread_enabled() {
    const std::optional<std::string> value = g6shim::settings::get("G6SHIM_COMPLETION_THREAD");
    if (!value || value->empty()) return true;
    return value->front() != '0';
}


void begin_queue_submission(Object* queue) { t_queue = queue; }

Object* submitting_queue() { return t_queue; }

void wait_for_writes(const void* address, std::size_t bytes) {
    if (!completion_thread_enabled() || address == nullptr || bytes == 0) {
        return;
    }
    const auto start = reinterpret_cast<std::size_t>(address);
    Lane& owned = lane();
    for (;;) {
        std::uint64_t need = 0;
        {
            std::unique_lock<std::mutex> lock(owned.mutex);
            for (const Flight& flight : owned.flights) {
                for (const auto& [shadow, write_at, write_bytes] : flight.writes) {
                    if (shadow && overlaps(start, bytes, write_at, write_bytes))
                        need = std::max(need, flight.host_value);
                }
            }
            if (need == 0 || owned.landed >= need || owned.abandon.load(std::memory_order_acquire) ||
                owned.stop.load(std::memory_order_acquire) || device_removed())
                return;
            owned.cv.wait_for(lock, std::chrono::milliseconds(kWaitSliceMs));
        }
    }
}

void wait_idle() {
    Lane& owned = lane();
    if (!owned.started.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lock(owned.mutex);
    owned.cv.wait(lock, [&] {
        return owned.flights.empty() && !owned.retiring.load(std::memory_order_acquire);
    });
}

std::uint32_t in_flight_for_test() {
    Lane& owned = lane();
    std::lock_guard<std::mutex> lock(owned.mutex);
    return static_cast<std::uint32_t>(owned.flights.size());
}

void pause_completion_for_test(bool pause) {
    Lane& owned = lane();
    owned.paused.store(pause, std::memory_order_release);
    owned.cv.notify_all();
}

void abandon_in_flight_for_test() {
    Lane& owned = lane();
    owned.abandon.store(true, std::memory_order_release);
    owned.paused.store(false, std::memory_order_release);
    if (owned.wake != nullptr) ::SetEvent(owned.wake);
    owned.cv.notify_all();
    std::unique_lock<std::mutex> lock(owned.mutex);
    owned.cv.wait(lock, [&] {
        return owned.flights.empty() && !owned.retiring.load(std::memory_order_acquire) &&
               !owned.abandon.load(std::memory_order_acquire);
    });
}

void set_device_removed_for_test(bool removed) {
    Lane& owned = lane();
    owned.removed.store(removed, std::memory_order_release);
    if (removed && owned.wake != nullptr) ::SetEvent(owned.wake);
    owned.cv.notify_all();
}

void note_queue_signal(Object* queue, Object* fence_object, std::uint64_t value) {
    if (fence_object == nullptr) return;
    if (!completion_thread_enabled()) {
        fence::signal(fence_object, value);
        return;
    }
    ensure_started();
    Lane& owned = lane();
    bool now = false;
    {
        std::lock_guard<std::mutex> lock(owned.mutex);
        const std::uint64_t after = queue != nullptr ? owned.queues[queue].last_flight : 0;
        if (after == 0 || after <= owned.landed) now = true;
        else owned.signals.push_back(PendingSignal{after, fence_object, value});
    }
    if (now) fence::signal(fence_object, value);
}

void note_queue_wait(Object* queue, Object* fence_object, std::uint64_t value) {
    if (!completion_thread_enabled() || queue == nullptr || fence_object == nullptr) return;
    ensure_started();
    Lane& owned = lane();
    std::lock_guard<std::mutex> lock(owned.mutex);
    owned.queues[queue].waits.emplace_back(fence_object, value);
}

void await_queue_waits(Object* queue) {
    if (!completion_thread_enabled() || queue == nullptr) return;
    Lane& owned = lane();
    for (;;) {
        std::vector<std::pair<Object*, std::uint64_t>> pending;
        {
            std::lock_guard<std::mutex> lock(owned.mutex);
            if (owned.abandon.load(std::memory_order_acquire) || owned.stop.load(std::memory_order_acquire) ||
                device_removed())
                return;
            QueueTrack& track = owned.queues[queue];
            for (const auto& wait : track.waits)
                if (wait.first != nullptr && fence_value(wait.first) < wait.second) pending.push_back(wait);
            track.waits.clear();
            if (pending.empty()) return;
        }
        for (const auto& [fence_object, value] : pending) {
            if (fence_object == nullptr) continue;
            HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event == nullptr) return;
            const auto set_event = slot<HRESULT (*)(Object*, std::uint64_t, void*)>(fence_object, 0x48);
            if (FAILED(set_event(fence_object, value, event))) {
                ::CloseHandle(event);
                return;
            }
            while (fence_value(fence_object) < value && !owned.abandon.load(std::memory_order_acquire) &&
                   !owned.stop.load(std::memory_order_acquire) && !device_removed())
                ::WaitForSingleObject(event, kWaitSliceMs);
            ::CloseHandle(event);
            if (owned.abandon.load(std::memory_order_acquire) || owned.stop.load(std::memory_order_acquire) ||
                device_removed())
                return;
        }
    }
}

void enqueue_flight(FlightInput flight) {
    ensure_started();
    Lane& owned = lane();
    Flight stored;
    stored.host_value = flight.host_value;
    stored.writes = std::move(flight.writes);
    stored.deferred = std::move(flight.deferred);
    stored.kept = std::move(flight.kept);
    stored.cards = std::move(flight.cards);
    stored.cards_open = flight.cards_open;
    stored.timing = flight.timing;
    {
        std::unique_lock<std::mutex> lock(owned.mutex);
        owned.cv.wait(lock, [&] {
            return owned.stop.load(std::memory_order_acquire) || owned.abandon.load(std::memory_order_acquire) ||
                   device_removed() || owned.flights.size() < kCompletionDepth;
        });
        if (owned.stop.load(std::memory_order_acquire) || owned.abandon.load(std::memory_order_acquire) ||
            device_removed()) {
            auto writes = std::move(stored.writes);
            lock.unlock();
            release_cards(stored);
            discard_writes(writes);
            return;
        }
        if (flight.queue != nullptr) owned.queues[flight.queue].last_flight = flight.host_value;
        owned.flights.push_back(std::move(stored));
    }
    owned.cv.notify_all();
}


void stop_completion() {
    Lane& owned = lane();
    if (!owned.started.load(std::memory_order_acquire)) return;
    owned.stop.store(true, std::memory_order_release);
    owned.paused.store(false, std::memory_order_release);
    if (owned.wake != nullptr) ::SetEvent(owned.wake);
    owned.cv.notify_all();
    if (owned.thread.joinable()) owned.thread.join();
    if (owned.fence_event != nullptr) {
        ::CloseHandle(owned.fence_event);
        owned.fence_event = nullptr;
    }
    if (owned.wake != nullptr) {
        ::CloseHandle(owned.wake);
        owned.wake = nullptr;
    }
}

}  // namespace g6shim::d3d12_xs::host
