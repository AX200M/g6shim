// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xasync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <thread>

namespace {

namespace xa = g6shim::xgameruntime::xasync;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;

using Create = HResult (*)(Object*, std::uint32_t, std::uint32_t, void**);
using Submit = HResult (*)(Object*, const void*, std::uint32_t, void*, xa::Callback);
using Dispatch = bool (*)(Object*, const void*, std::uint32_t, std::uint32_t);
using Close = void (*)(Object*, const void*);
using Run = HResult (*)(Object*, xa::AsyncBlock*, xa::Work);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(xa::kObject, offset);
}

Object* self() { return const_cast<Object*>(&xa::kObject); }

std::atomic<std::uint32_t> g_ran{0};

void count(void* context, bool canceled) {
    const std::uint32_t value = canceled ? 100 : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(context));
    g_ran.fetch_add(value, std::memory_order_seq_cst);
}

HResult scheduling_provider(std::uint32_t operation, const xa::ProviderData* data) {
    switch (operation) {
        case xa::kOpBegin:
            return xa::XAsyncSchedule(data->block, 0);
        case xa::kOpDoWork:
            xa::XAsyncComplete(data->block, g6shim::xgameruntime::kOk, 8);
            return g6shim::xgameruntime::kOk;
        case xa::kOpGetResult:
            *static_cast<std::uint64_t*>(data->buffer) =
                static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data->context));
            return g6shim::xgameruntime::kOk;
        default:
            return g6shim::xgameruntime::kOk;
    }
}

std::atomic<std::uint32_t> g_completed{0};

void completed(xa::AsyncBlock*) { g_completed.fetch_add(1, std::memory_order_seq_cst); }

HResult aborting_work(xa::AsyncBlock*) { return g6shim::xgameruntime::kAbort; }

std::atomic<bool> g_second{false};
std::atomic<bool> g_first{false};

void waits_for_second(void*, bool) {
    const auto start = std::chrono::steady_clock::now();
    while (!g_second.load(std::memory_order_seq_cst) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::yield();
    g_first.store(g_second.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
}

void releases_second(void*, bool) { g_second.store(true, std::memory_order_seq_cst); }

std::atomic<std::uint32_t> g_dispatched{0};

void dispatched(void*, bool) { g_dispatched.fetch_add(1, std::memory_order_seq_cst); }

std::atomic<bool> g_terminated{false};

void terminated(void*) { g_terminated.store(true, std::memory_order_seq_cst); }

void note_canceled(void* context, bool canceled) { *static_cast<bool*>(context) = canceled; }

constexpr HResult kSFalse = 1;

HResult s_false_provider(std::uint32_t operation, const xa::ProviderData* data) {
    switch (operation) {
        case xa::kOpBegin:
            return xa::XAsyncSchedule(data->block, 0);
        case xa::kOpDoWork:
            xa::XAsyncComplete(data->block, kSFalse, 8);
            return g6shim::xgameruntime::kOk;
        case xa::kOpGetResult:
            *static_cast<std::uint64_t*>(data->buffer) = 0x1234;
            return g6shim::xgameruntime::kOk;
        default:
            return g6shim::xgameruntime::kOk;
    }
}

}  // namespace

TEST(xgameruntime_xasync_manual_ports_run_callbacks_when_dispatched) {
    void* queue = nullptr;
    CHECK(at<Create>(0x60)(self(), 0, 0, &queue) == g6shim::xgameruntime::kOk);
    CHECK(at<Submit>(0x90)(self(), queue, xa::kCompletion, reinterpret_cast<void*>(std::uintptr_t{5}), &count) ==
          g6shim::xgameruntime::kOk);
    CHECK(g_ran.load() == 0);
    CHECK(!at<Dispatch>(0x80)(self(), queue, xa::kWork, 0));
    CHECK(at<Dispatch>(0x80)(self(), queue, xa::kCompletion, 0));
    CHECK(g_ran.load() == 5);
    CHECK(!at<Dispatch>(0x80)(self(), queue, xa::kCompletion, 0));
    at<Close>(0x88)(self(), queue);
}

TEST(xgameruntime_xasync_async_calls_complete_and_return_results) {
    void* queue = nullptr;
    CHECK(xa::XTaskQueueCreate(1, 0, &queue) == g6shim::xgameruntime::kOk);
    xa::AsyncBlock block{};
    block.queue = queue;
    block.callback = &completed;
    const void* identity = reinterpret_cast<const void*>(std::uintptr_t{0x1d});
    CHECK(xa::XAsyncBegin(&block, reinterpret_cast<void*>(std::uintptr_t{42}), identity, nullptr,
                          &scheduling_provider) == g6shim::xgameruntime::kOk);
    CHECK(xa::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    std::size_t size = 0;
    CHECK(xa::XAsyncGetResultSize(&block, &size) == g6shim::xgameruntime::kOk);
    CHECK(size == 8);
    CHECK(g_completed.load() == 0);
    CHECK(xa::XTaskQueueDispatch(queue, xa::kCompletion, xa::kInfinite));
    CHECK(g_completed.load() == 1);

    std::uint32_t small = 0;
    CHECK(xa::XAsyncGetResult(&block, identity, 4, &small, nullptr) == g6shim::xgameruntime::kNotSufficientBuffer);
    std::uint64_t value = 0;
    std::size_t used = 0;
    CHECK(xa::XAsyncGetResult(&block, identity, 8, &value, &used) == g6shim::xgameruntime::kOk);
    CHECK(value == 42 && used == 8);
    CHECK(xa::XAsyncGetResult(&block, identity, 8, &value, nullptr) == g6shim::xgameruntime::kIllegalMethodCall);
    xa::XTaskQueueCloseHandle(queue);
}

TEST(xgameruntime_xasync_a_nonnegative_completion_still_returns_its_payload) {
    void* queue = nullptr;
    CHECK(xa::XTaskQueueCreate(0, 0, &queue) == g6shim::xgameruntime::kOk);
    xa::AsyncBlock block{};
    block.queue = queue;
    const void* identity = reinterpret_cast<const void*>(std::uintptr_t{0x77});
    CHECK(xa::XAsyncBegin(&block, nullptr, identity, nullptr, &s_false_provider) ==
          g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueDispatch(queue, xa::kWork, 0));
    CHECK(xa::XAsyncGetStatus(&block, false) == kSFalse);
    std::size_t size = 0;
    CHECK(xa::XAsyncGetResultSize(&block, &size) == kSFalse);
    CHECK(size == 8);
    std::uint64_t value = 0;
    std::size_t used = 0;
    CHECK(xa::XAsyncGetResult(&block, identity, 8, &value, &used) == g6shim::xgameruntime::kOk);
    CHECK(value == 0x1234 && used == 8);
    CHECK(xa::XAsyncGetResult(&block, identity, 8, &value, nullptr) == g6shim::xgameruntime::kIllegalMethodCall);
    xa::XTaskQueueCloseHandle(queue);
}

TEST(xgameruntime_xasync_run_returns_the_work_result) {
    xa::AsyncBlock block{};
    CHECK(at<Run>(0x30)(self(), &block, &aborting_work) == g6shim::xgameruntime::kOk);
    CHECK(xa::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kAbort);
}

TEST(xgameruntime_xasync_pool_callbacks_may_wait_for_each_other) {
    g_first.store(false);
    g_second.store(false);
    void* queue = nullptr;
    CHECK(xa::XTaskQueueCreate(1, 1, &queue) == g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueSubmitDelayedCallback(queue, xa::kWork, 0, nullptr, &waits_for_second) ==
          g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueSubmitDelayedCallback(queue, xa::kWork, 0, nullptr, &releases_second) ==
          g6shim::xgameruntime::kOk);
    const auto start = std::chrono::steady_clock::now();
    while (!g_first.load(std::memory_order_seq_cst) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_first.load());
    xa::XTaskQueueCloseHandle(queue);
}

TEST(xgameruntime_xasync_pending_with_completes_when_its_work_returns) {
    xa::AsyncBlock block{};
    const void* identity = reinterpret_cast<const void*>(std::uintptr_t{0x5a});
    CHECK(xa::pending_with(&block, identity, [] {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
              return xa::PendingResult{g6shim::xgameruntime::kOk, xa::bytes(std::uint64_t{0x9876})};
          }) == g6shim::xgameruntime::kOk);
    CHECK(xa::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    std::uint64_t value = 0;
    CHECK(xa::result<std::uint64_t>(&block, identity, &value) == g6shim::xgameruntime::kOk);
    CHECK(value == 0x9876);
}

TEST(xgameruntime_xasync_terminate_ends_a_waiting_dispatch) {
    g_dispatched.store(0);
    void* first_queue = nullptr;
    void* second_queue = nullptr;
    CHECK(xa::XTaskQueueCreate(0, 0, &first_queue) == g6shim::xgameruntime::kOk);
    void* work_port = nullptr;
    void* completion_port = nullptr;
    CHECK(xa::XTaskQueueGetPort(first_queue, xa::kWork, &work_port) == g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueGetPort(first_queue, xa::kCompletion, &completion_port) == g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueCreateComposite(work_port, completion_port, &second_queue) ==
          g6shim::xgameruntime::kOk);

    std::promise<void> ended;
    std::future<void> finished = ended.get_future();
    std::thread dispatcher([&] {
        while (xa::XTaskQueueDispatch(first_queue, xa::kWork, xa::kInfinite)) {
        }
        ended.set_value();
    });

    const auto run_one = [&](void* queue, std::uint32_t total) {
        CHECK(xa::XTaskQueueSubmitDelayedCallback(queue, xa::kWork, 0, nullptr, &dispatched) ==
              g6shim::xgameruntime::kOk);
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (g_dispatched.load() == total) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(false);
    };

    run_one(first_queue, 1);
    CHECK(xa::XTaskQueueTerminate(second_queue, false, nullptr, nullptr) == g6shim::xgameruntime::kOk);
    CHECK(finished.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
    run_one(first_queue, 2);
    CHECK(xa::XTaskQueueTerminate(first_queue, false, nullptr, nullptr) == g6shim::xgameruntime::kOk);
    CHECK(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(!xa::XTaskQueueDispatch(first_queue, xa::kWork, xa::kInfinite));
    dispatcher.join();
    xa::XTaskQueueCloseHandle(first_queue);
    xa::XTaskQueueCloseHandle(second_queue);
}

TEST(xgameruntime_xasync_terminate_cancels_what_is_queued) {
    g_terminated.store(false);
    void* queue = nullptr;
    CHECK(xa::XTaskQueueCreate(0, 0, &queue) == g6shim::xgameruntime::kOk);
    bool canceled = false;
    CHECK(xa::XTaskQueueSubmitDelayedCallback(queue, xa::kWork, 0, &canceled, &note_canceled) ==
          g6shim::xgameruntime::kOk);
    CHECK(xa::XTaskQueueTerminate(queue, true, nullptr, &terminated) == g6shim::xgameruntime::kOk);
    CHECK(canceled && g_terminated.load());
    CHECK(xa::XTaskQueueSubmitDelayedCallback(queue, xa::kWork, 0, &canceled, &note_canceled) ==
          g6shim::xgameruntime::kAbort);
    xa::XTaskQueueCloseHandle(queue);
}
