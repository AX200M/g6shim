// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "console_objects.hpp"
#include "frame_clock.hpp"

#include <g6shim/log.hpp>

#include <array>
#include <mutex>
#include <thread>
#include <vector>

namespace g6shim::d3d12 {

namespace {

constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;
constexpr LONGLONG kHundredNanosecondsPerMicrosecond = 10;

class FramePacer {
public:
    HRESULT set_interval(std::uint32_t interval_us, std::uint32_t periods) {
        if (!frame_interval_valid(interval_us, periods)) return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(mutex_);
        clock_.interval_us = interval_us;
        clock_.origin_period = periods;
        anchor();
        return S_OK;
    }

    HRESULT schedule(std::uint32_t type, std::uint32_t offset_us, const HANDLE* objects, std::uint32_t count,
                     std::uint32_t flags) {
        if (type >= kFrameEventTypes || count > kMaxScheduledObjects || (count && !objects)) return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!start()) return E_OUTOFMEMORY;
        clock_.offset_us[type] = offset_us;
        handles_[type].clear();
        if (!(flags & kScheduleFlagDisableSignal)) handles_[type].assign(objects, objects + count);
        next_[type] = clock_.first_index_at_or_after(type, elapsed_us());
        SetEvent(wake_);
        return S_OK;
    }

private:
    bool start() {
        if (started_) return true;
        QueryPerformanceFrequency(&frequency_);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!wake_ || !timer_) return false;
        anchor();
        std::thread([this] { run(); }).detach();
        started_ = true;
        return true;
    }

    void anchor() {
        QueryPerformanceCounter(&origin_);
        for (std::uint32_t type = 0; type < kFrameEventTypes; ++type) next_[type] = 0;
        if (wake_) SetEvent(wake_);
    }

    std::uint64_t elapsed_us() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!frequency_.QuadPart) return 0;
        return static_cast<std::uint64_t>(now.QuadPart - origin_.QuadPart) * kMicrosecondsPerSecond /
               static_cast<std::uint64_t>(frequency_.QuadPart);
    }

    bool next_due(std::uint64_t* due_us) const {
        bool any = false;
        for (std::uint32_t type = 0; type < kFrameEventTypes; ++type) {
            if (handles_[type].empty()) continue;
            const std::uint64_t at = clock_.event_time_us(type, next_[type]);
            if (!any || at < *due_us) *due_us = at;
            any = true;
        }
        return any;
    }

    void signal_due() {
        const std::uint64_t now = elapsed_us();
        for (std::uint32_t type = 0; type < kFrameEventTypes; ++type) {
            if (handles_[type].empty() || clock_.event_time_us(type, next_[type]) > now) continue;
            for (HANDLE h : handles_[type]) SetEvent(h);
            next_[type] = clock_.first_index_at_or_after(type, now + 1);
        }
    }

    void run() {
        const HANDLE waits[] = {timer_, wake_};
        for (;;) {
            std::uint64_t due = 0;
            std::uint64_t now = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!next_due(&due)) due = UINT64_MAX;
                now = elapsed_us();
            }
            if (due == UINT64_MAX) {
                WaitForSingleObject(wake_, INFINITE);
                continue;
            }
            if (due > now) {
                LARGE_INTEGER relative{};
                relative.QuadPart = -static_cast<LONGLONG>(due - now) * kHundredNanosecondsPerMicrosecond;
                SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE);
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) continue;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            signal_due();
        }
    }

    std::mutex mutex_;
    FrameClock clock_;
    std::array<std::vector<HANDLE>, kFrameEventTypes> handles_;
    std::array<std::uint64_t, kFrameEventTypes> next_{};
    LARGE_INTEGER origin_{};
    LARGE_INTEGER frequency_{};
    HANDLE wake_ = nullptr;
    HANDLE timer_ = nullptr;
    bool started_ = false;
};

FramePacer& pacer() {
    static FramePacer* instance = new FramePacer;
    return *instance;
}

}  // namespace

HRESULT STDMETHODCALLTYPE device_set_frame_interval(ConsoleDevice*, void* output, UINT interval_us, UINT periods,
                                                    UINT flags) {
    const HRESULT hr = pacer().set_interval(interval_us, periods);
    G6SHIM_INFO("SetFrameIntervalX(output %p, %u us, %u periods, flags 0x%X) -> 0x%08lX", output, interval_us, periods,
                flags, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE device_schedule_frame_event(ConsoleDevice*, UINT type, UINT offset_us,
                                                      const ConsoleObjectList* objects, UINT flags) {
    const std::uint32_t count = objects ? objects->count : 0;
    const HRESULT hr = pacer().schedule(type, offset_us, objects ? objects->objects : nullptr, count, flags);
    G6SHIM_INFO("ScheduleFrameEventX(type %u, offset %u us, %u objects, flags 0x%X) -> 0x%08lX", type, offset_us, count,
                flags, hr);
    return hr;
}

}  // namespace g6shim::d3d12
