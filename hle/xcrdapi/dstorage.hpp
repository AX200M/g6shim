// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "objects.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace g6shim::xcrdapi {

inline constexpr HRESULT kEndOfFile = static_cast<HRESULT>(0x80070026u);
inline constexpr HRESULT kInvalidData = static_cast<HRESULT>(0x8007000Du);

inline constexpr std::uint64_t kOptionZlib = 1;
inline constexpr std::uint64_t kOptionBcpack = 0xfu << 1;
inline constexpr std::uint64_t kOptionPageDestination = std::uint64_t{1} << 21;
inline constexpr std::uint64_t kOptionMemorySource = std::uint64_t{1} << 22;

inline constexpr std::size_t kDestinationPageBytes = 64 * 1024;
inline constexpr std::uint64_t kDefaultStagingBytes = std::uint64_t{32} << 20;
inline constexpr std::array<std::uint64_t, 3> kPriorityCost{100, 10, 1};

struct Request {
    std::uint64_t options;
    void* destination;
    std::uint64_t page_offset;
    std::uint32_t destination_size;
    const void* source;
    std::uint64_t offset;
    std::uint32_t source_size;
    std::uint32_t intermediate_size;
    std::uint64_t tag;
    const char* name;
};

struct QueueDesc {
    std::uint64_t source_type;
    std::uint16_t capacity;
    std::int8_t priority;
    const char* name;
    std::uint64_t unknown;
};

struct StatusArray {
    std::vector<std::atomic<HRESULT>> entries;

    explicit StatusArray(std::uint32_t capacity);
    std::size_t size() const { return entries.size(); }
    std::atomic<HRESULT>& at(std::uint32_t index) { return entries[index]; }
};

struct Status {
    Instance<StatusArray>* array = nullptr;
    std::uint32_t index = 0;

    Status() = default;
    Status(Instance<StatusArray>* target, std::uint32_t entry) : array(target), index(entry) {}
    Status(const Status&) = delete;
    Status& operator=(const Status&) = delete;
    Status(Status&& other) noexcept : array(std::exchange(other.array, nullptr)), index(other.index) {}
    Status& operator=(Status&& other) noexcept {
        if (this != &other) {
            release();
            array = std::exchange(other.array, nullptr);
            index = other.index;
        }
        return *this;
    }
    ~Status() { release(); }

    void release() {
        if (array != nullptr) release_instance(array);
        array = nullptr;
    }
    std::atomic<HRESULT>& entry() { return array->state.at(index); }
};

using Command = std::variant<Request, Status>;

struct Lane {
    std::size_t class_index = 0;
    std::size_t capacity = 0;
    std::uint64_t clock = 0;
    std::vector<Command> unsubmitted;
    std::deque<std::pair<std::uint64_t, Request>> reads;
    std::deque<std::pair<std::uint64_t, Status>> statuses;
    std::set<std::uint64_t> running;
    std::map<std::uint64_t, HRESULT> failures;
    bool closed = false;

    static Lane make(std::int8_t priority, std::uint16_t capacity);
    std::size_t entries() const;
    void flush();
};

struct Job {
    std::uint64_t lane;
    std::uint64_t seq;
    Request request;
    std::uint64_t staging;
};

struct WorkStats {
    std::uint64_t requests = 0;
    std::uint64_t bytes = 0;
    std::chrono::nanoseconds busy{};
};

struct Lanes {
    std::unordered_map<std::uint64_t, Lane> lanes;
    std::uint64_t next_id = 0;
    std::uint64_t next_seq = 0;
    std::uint64_t staged = 0;
    std::uint64_t staging = kDefaultStagingBytes;
    bool starved = false;
    std::array<std::uint64_t, 3> clock{};
    std::uint64_t now = 0;
    std::array<std::uint64_t, 3> lane_now{};
    WorkStats done;
    std::uint64_t refused = 0;
    std::pair<std::uint64_t, std::chrono::nanoseconds> bcpack{};
    std::optional<std::chrono::steady_clock::time_point> since;

    std::optional<Job> start();
    void finish(const Job& job, HRESULT result);
    void submit(std::uint64_t id);
    void count(const Job& job, std::chrono::steady_clock::time_point started);
};

struct Scheduler {
    std::mutex mutex;
    std::condition_variable work;
    std::condition_variable finished;
    Lanes lanes;
};

Scheduler* scheduler();

std::uint64_t staging_bytes(const Request& request);
void close_lane(std::uint64_t id);
void use_bcpack_tables(std::span<const std::uint8_t> bytes);

}  // namespace g6shim::xcrdapi
