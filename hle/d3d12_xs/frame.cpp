// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace g6shim::d3d12_xs::frame {

namespace {

constexpr HRESULT kTimeout = S_FALSE;
constexpr std::uint64_t kSixtiethNanoseconds = 16'667'000;
constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::uint64_t kTimerUnitsPerNanosecondDenominator = 100;
constexpr std::size_t kKinds = 3;

struct Clock {
    std::chrono::steady_clock::time_point start;
    std::uint64_t interval;
    std::uint64_t first;
};

std::mutex g_clock_mutex;
std::optional<Clock> g_clock;
std::atomic<std::uint64_t> g_frame{0};
std::array<std::atomic<std::uint64_t>, kKinds> g_last{};

std::uint64_t now_nanoseconds(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::atomic<std::uint64_t>& last(std::uint32_t kind) {
    return g_last[std::min<std::size_t>(kind, kKinds - 1)];
}

void bump_last(std::uint32_t kind, std::uint64_t tick) {
    auto& value = last(kind);
    std::uint64_t current = value.load(std::memory_order_relaxed);
    while (current < tick &&
           !value.compare_exchange_weak(current, tick, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void sleep_nanoseconds(std::uint64_t nanoseconds) {
    if (nanoseconds == 0) return;
    thread_local HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                         TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    const std::uint64_t units = nanoseconds / kTimerUnitsPerNanosecondDenominator + 1;
    due.QuadPart = -static_cast<LONGLONG>(units);
    if (timer != nullptr && ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        ::WaitForSingleObject(timer, INFINITE);
        return;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
}

std::pair<std::uint64_t, std::uint64_t> next_tick(std::uint32_t kind) {
    std::lock_guard<std::mutex> lock(g_clock_mutex);
    if (!g_clock) g_clock = Clock{std::chrono::steady_clock::now(), kSixtiethNanoseconds, 0};
    const Clock& clock = *g_clock;
    const std::uint64_t elapsed = now_nanoseconds(clock.start);
    const std::uint64_t interval = std::max<std::uint64_t>(clock.interval, 1);
    const std::uint64_t from_clock = clock.first + elapsed / interval + 1;
    const std::uint64_t tick = std::max(from_clock, last(kind).load(std::memory_order_relaxed) + 1);
    const std::uint64_t delta = tick - clock.first;
    const std::uint64_t at =
        delta > std::numeric_limits<std::uint64_t>::max() / interval ? std::numeric_limits<std::uint64_t>::max()
                                                                    : delta * interval;
    return {tick, at > elapsed ? at - elapsed : 0};
}

}  // namespace

HRESULT set_interval(Object*, Object*, std::uint32_t length_microseconds, std::uint32_t, std::uint32_t) {
    const std::uint64_t interval = length_microseconds == 0
                                       ? kSixtiethNanoseconds
                                       : static_cast<std::uint64_t>(length_microseconds) * 1'000;
    std::uint64_t first = 0;
    for (auto& value : g_last) first = std::max(first, value.load(std::memory_order_relaxed));
    {
        std::lock_guard<std::mutex> lock(g_clock_mutex);
        g_clock = Clock{std::chrono::steady_clock::now(), interval, first};
    }
    G6SHIM_INFO("d3d12_xs: frame interval %llu us", static_cast<unsigned long long>(interval / 1'000));
    return S_OK;
}

HRESULT schedule(Object*, std::uint32_t, std::uint32_t, const std::uint8_t*, std::uint32_t) { return S_OK; }

std::uint64_t current_frame() { return g_frame.load(std::memory_order_relaxed); }

std::uint64_t last_tick(std::uint32_t kind) { return last(kind).load(std::memory_order_relaxed); }

std::uint64_t next_tick_number(std::uint32_t kind) { return next_tick(kind).first; }

HRESULT wait(Object*, std::uint32_t kind, std::uint32_t timeout_ms, const std::uint8_t*, std::uint32_t flags,
             std::uint64_t* token) {
    const bool origin = kind == kOrigin;
    const auto [tick, wait] = next_tick(kind);
    const std::uint64_t limit = static_cast<std::uint64_t>(timeout_ms) * kNanosecondsPerMillisecond;
    HRESULT hr = S_OK;
    if (wait > limit) {
        sleep_nanoseconds(limit);
        hr = kTimeout;
    } else {
        sleep_nanoseconds(wait);
        bump_last(kind, tick);
    }
    if (origin && (hr == S_OK || (flags & kTokenOnTimeout) != 0)) {
        const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (token != nullptr) *token = frame;
    }
    return hr;
}

HRESULT statistics(Object*, std::uint64_t, std::uint32_t, std::uint32_t* count, void*) {
    if (count != nullptr) *count = 0;
    return S_OK;
}

}  // namespace g6shim::d3d12_xs::frame
