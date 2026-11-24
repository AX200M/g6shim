// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_make.hpp"

#include "frame.hpp"
#include "pipeline.hpp"
#include "pipeline_library.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/emit/container.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>

namespace g6shim::d3d12_xs::make {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::microseconds kBudget{500'000};

std::mutex g_done_mutex;
std::condition_variable g_done;
std::atomic<std::uint64_t> g_waiting{0};
std::atomic<std::uint64_t> g_peak{0};
std::atomic<std::uint64_t> g_frame{UINT64_MAX};
std::atomic<std::uint64_t> g_waited{0};

struct Queue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::shared_ptr<Pending>> items;
};

std::atomic<unsigned> g_workers{0};

void drained();
void queue(const std::shared_ptr<Pending>& pending);

std::string key_for(const std::vector<std::shared_ptr<Shader>>& shaders,
                    const pipeline_host::GraphicsState* graphics) {
    std::vector<std::uint8_t> bytes;
    const auto append = [&bytes](std::string_view text) {
        bytes.insert(bytes.end(), text.begin(), text.end());
    };
    for (const auto& shader : shaders) {
        append(shader->stage);
        append(shader->hash);
    }
    if (graphics != nullptr) append(graphics->describe());
    const std::array<std::uint8_t, 16> digest = g6shim::shader::emit::hash(bytes);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

void wait_for(const std::shared_ptr<Pending>& pending, std::uint64_t frame) {
    if (g_frame.exchange(frame, std::memory_order_relaxed) != frame)
        g_waited.store(0, std::memory_order_relaxed);
    const std::uint64_t waited = g_waited.load(std::memory_order_relaxed);
    const std::chrono::microseconds left =
        waited >= static_cast<std::uint64_t>(kBudget.count())
            ? std::chrono::microseconds{0}
            : kBudget - std::chrono::microseconds{static_cast<long long>(waited)};
    const Clock::time_point started = Clock::now();
    std::unique_lock<std::mutex> held(g_done_mutex);
    while (!pending->done()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
        if (elapsed >= left) break;
        g_done.wait_for(held, left - elapsed);
    }
    const auto used = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
    g_waited.fetch_add(static_cast<std::uint64_t>(used.count()), std::memory_order_relaxed);
}

void worker_loop(const std::shared_ptr<Queue>& state) {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::shared_ptr<Pending> next;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [&state] { return !state->items.empty(); });
            next = std::move(state->items.front());
            state->items.pop_front();
        }
        next->make();
        if (g_waiting.fetch_sub(1, std::memory_order_relaxed) == 1)
            drained();
        else
            pipeline_library::save(false);
    }
}

void queue(const std::shared_ptr<Pending>& pending) {
    static const std::shared_ptr<Queue> state = std::make_shared<Queue>();
    static std::once_flag once;
    std::call_once(once, [] {
        static int anchor = 0;
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&anchor), &self);
        const unsigned hardware = std::thread::hardware_concurrency();
        const unsigned count = std::clamp(hardware / 2, 2u, 8u);
        unsigned spawned = 0;
        for (unsigned i = 0; i < count; ++i) {
            try {
                std::thread(worker_loop, state).detach();
                ++spawned;
            } catch (const std::exception& error) {
                G6SHIM_WARN("d3d12_xs: no pipeline worker: %s", error.what());
            }
        }
        g_workers.store(spawned, std::memory_order_release);
    });
    if (g_workers.load(std::memory_order_acquire) == 0) {
        pending->make();
        g_waiting.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->items.push_back(pending);
    }
    state->ready.notify_one();
}

void start(const std::shared_ptr<Pending>& pending) {
    if (!pending->mark_queued()) return;
    const std::uint64_t now = g_waiting.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t peak = g_peak.load(std::memory_order_relaxed);
    while (peak < now &&
           !g_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    queue(pending);
}

void drained() {
    if (g_peak.exchange(0, std::memory_order_relaxed) >= 10)
        G6SHIM_INFO("d3d12_xs: host pipelines all made (made %llu, loaded %llu, started early %llu)",
                    static_cast<unsigned long long>(pipeline_library::made.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(pipeline_library::loaded.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(pipeline_library::early.load(std::memory_order_relaxed)));
    pipeline_library::save(false);
}

}  // namespace

Pending::Pending(std::vector<std::shared_ptr<Shader>> pipeline_shaders,
                 std::shared_ptr<pipeline_host::GraphicsState> state)
    : shaders(std::move(pipeline_shaders)),
      graphics(std::move(state)),
      key(key_for(shaders, graphics.get())) {}

void Pending::make() {
    std::call_once(once_, [this] {
        try {
            std::optional<pipeline_host::HostPipeline> made =
                pipeline_host::HostPipeline::make(shaders, graphics.get());
            if (made) made_.emplace(std::move(*made));
        } catch (const std::exception& error) {
            G6SHIM_WARN("d3d12_xs: a host pipeline was not made: %s", error.what());
        } catch (...) {
            G6SHIM_WARN("d3d12_xs: a host pipeline was not made");
        }
        done_.store(true, std::memory_order_release);
    });
    {
        std::lock_guard<std::mutex> lock(g_done_mutex);
    }
    g_done.notify_all();
}

pipeline_host::HostPipeline* Pending::result() const {
    if (!done()) return nullptr;
    return made_ ? &*made_ : nullptr;
}

pipeline_host::HostPipeline* ready(const std::shared_ptr<Pending>& pending) {
    if (!pending->done()) {
        start(pending);
        wait_for(pending, frame::current_frame());
    }
    pipeline_host::HostPipeline* made = pending->result();
    if (made != nullptr && !pending->used.exchange(true)) pipeline_library::note_used(pending->key);
    return made;
}

void early(const std::shared_ptr<Pending>& pending) {
    if (!pending->shaders.empty() && pipeline_library::wanted_early(pending->key)) {
        pipeline_library::early.fetch_add(1, std::memory_order_relaxed);
        start(pending);
    }
}

std::uint64_t waiting() { return g_waiting.load(std::memory_order_relaxed); }

}  // namespace g6shim::d3d12_xs::make
