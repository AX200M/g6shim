// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "bcpack.hpp"
#include "dstorage.hpp"
#include "objects.hpp"
#include "virtual_memory.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace g6shim::xcrdapi;

extern "C" HRESULT WINAPI g6shim_DStorageGetFactory(const Guid* iid, void** out);
extern "C" void* WINAPI g6shim_XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type,
                                                ULONGLONG flags, DWORD protect);
extern "C" BOOL WINAPI g6shim_XMemAllocatePhysicalPages(ULONGLONG flags, SIZE_T* count,
                                                        SIZE_T* pages);
extern "C" BOOL WINAPI g6shim_XMemFreePhysicalPages(SIZE_T count, const SIZE_T* pages);
extern "C" void* WINAPI g6shim_XMemMapPhysicalPages(void* address, SIZE_T count,
                                                    const SIZE_T* pages);

extern "C" void* WINAPI XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type, ULONGLONG flags,
                                         DWORD protect) {
    return g6shim_XMemVirtualAlloc(base, bytes, type, flags, protect);
}

extern "C" void* WINAPI XMemMapPhysicalPages(void* address, SIZE_T count, const SIZE_T* pages) {
    return g6shim_XMemMapPhysicalPages(address, count, pages);
}

namespace {

constexpr Guid kFactoryInterface{0x62959d11u, 0xd45f, 0x4884,
                                 {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

struct TempFile {
    std::wstring path;

    TempFile() {
        wchar_t directory[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        CHECK(length > 0 && length < MAX_PATH);
        wchar_t name[MAX_PATH]{};
        CHECK(GetTempFileNameW(directory, L"g6s", 0, name) != 0);
        path = name;
    }
    ~TempFile() {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept : path(std::exchange(other.path, std::wstring{})) {}
};

void write_file(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    CHECK(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0);
    CloseHandle(file);
}

void* factory() {
    void* object = nullptr;
    CHECK(g6shim_DStorageGetFactory(&kFactoryInterface, &object) == kSucceeded);
    return object;
}

void* open(const std::wstring& path) {
    using Open = HRESULT(WINAPI*)(void*, const wchar_t*, const Guid*, void**);
    const Open fn = method<Open>(factory(), 0x20);
    void* file = nullptr;
    CHECK(fn(factory(), path.c_str(), nullptr, &file) == kSucceeded);
    return file;
}

void* status_array(std::uint32_t capacity) {
    using Create = HRESULT(WINAPI*)(void*, std::uint32_t, const char*, const Guid*, void**);
    const Create fn = method<Create>(factory(), 0x28);
    void* array = nullptr;
    CHECK(fn(factory(), capacity, "test", nullptr, &array) == kSucceeded);
    return array;
}

void* new_queue(std::uint16_t capacity, std::int8_t priority, std::uint64_t source_type) {
    using Create = HRESULT(WINAPI*)(void*, const QueueDesc*, const Guid*, void**);
    const Create fn = method<Create>(factory(), 0x50);
    const QueueDesc desc{source_type, capacity, priority, "test", 1};
    void* queue = nullptr;
    CHECK(fn(factory(), &desc, nullptr, &queue) == kSucceeded);
    return queue;
}

Request request(std::uint64_t options, void* destination, std::size_t destination_size,
                const void* source, std::uint64_t offset, std::size_t source_size) {
    return Request{options, destination, 0, static_cast<std::uint32_t>(destination_size), source,
                   offset, static_cast<std::uint32_t>(source_size), 0, 0, nullptr};
}

void enqueue(void* queue, const Request& value) {
    using Enqueue = void(WINAPI*)(void*, const Request*);
    const Enqueue fn = method<Enqueue>(queue, 0x18);
    fn(queue, &value);
}

void enqueue_status(void* queue, void* array, std::uint32_t index) {
    using Enqueue = void(WINAPI*)(void*, void*, std::uint32_t);
    const Enqueue fn = method<Enqueue>(queue, 0x20);
    fn(queue, array, index);
}

void submit(void* queue) {
    using Submit = void(WINAPI*)(void*);
    const Submit fn = method<Submit>(queue, 0x30);
    fn(queue);
}

void close_queue(void* queue) {
    using Close = void(WINAPI*)(void*);
    const Close fn = method<Close>(queue, 0x40);
    fn(queue);
}

HRESULT result(void* array, std::uint32_t index) {
    using Result = HRESULT(WINAPI*)(void*, std::uint32_t);
    const Result fn = method<Result>(array, 0x20);
    return fn(array, index);
}

bool is_complete(void* array, std::uint32_t index) {
    using Complete = BOOL(WINAPI*)(void*, std::uint32_t);
    const Complete fn = method<Complete>(array, 0x18);
    return fn(array, index) != 0;
}

HRESULT wait(void* array, std::uint32_t index) {
    const auto started = std::chrono::steady_clock::now();
    while (!is_complete(array, index)) {
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result(array, index);
}

std::vector<std::uint8_t> pattern(std::size_t count) {
    std::vector<std::uint8_t> bytes(count, 0);
    for (std::size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<std::uint8_t>(i * 7 + i / 251);
    return bytes;
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& plain) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t count = std::min<std::size_t>(65535, plain.size() - at);
        const bool last = at + count == plain.size();
        out.push_back(last ? 1 : 0);
        const auto length = static_cast<std::uint16_t>(count);
        const auto complement = static_cast<std::uint16_t>(~length);
        out.push_back(static_cast<std::uint8_t>(length & 0xff));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(complement & 0xff));
        out.push_back(static_cast<std::uint8_t>(complement >> 8));
        out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(at),
                   plain.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
    } while (at < plain.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : plain) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = b << 16 | a;
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(adler >> (8 * k)));
    return out;
}

std::vector<std::uint8_t> test_tables() {
    std::vector<std::uint8_t> bytes;
    for (std::size_t m = 0; m < kBcpackModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < kBcpackBuckets; ++b) {
            const std::uint16_t f =
                b < g + 2 ? static_cast<std::uint16_t>(1u << (15 - g)) : std::uint16_t{0};
            bytes.push_back(static_cast<std::uint8_t>(f & 0xff));
            bytes.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v & 0x77));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v < 64; ++v)
        bytes.push_back(static_cast<std::uint8_t>((v & 7) | ((v >> 3) << 4)));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned code = 0; code < 512; ++code) {
        const std::uint16_t ctrl = code == 0 ? 0x46 : 0;
        bytes.push_back(static_cast<std::uint8_t>(ctrl & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(ctrl >> 8));
    }
    bytes.resize(kBcpackTablesSize, 0);
    return bytes;
}

void with_test_tables() { use_bcpack_tables(test_tables()); }

constexpr std::array<std::uint8_t, 88> kBc4Stream{
    0,    0,    0,    0,    0,    0,    0,    0,    5,    5,    5,    5,    4,    4,    4,    4,
    1,    1,    1,    1,    0x10, 0x10, 0x10, 0x10, 2,    1,    0,    0,    0,    0,    0,    0,
    0,    0,    0x14, 5,    0x20, 0x68, 0xb0, 0xf8, 3,    3,    2,    2,    0x24, 0x24, 0x24, 0x24,
    2,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    4,    4,    4,    4,
    0x10, 0x10, 0x10, 0x10, 0,    0,    0,    0,    0x20, 0x68, 0xb0, 0xf8, 0x24, 0x24, 0x24, 0x24,
    0,    0,    0,    0,    0,    0,    0,    0,
};

std::array<std::uint8_t, 16> bc4_blocks() {
    const std::uint64_t code[8] = {0, 2, 3, 4, 5, 6, 7, 1};
    const auto block = [&code](std::uint64_t e0, std::uint64_t e1, unsigned add) {
        std::uint64_t bits = 0;
        for (unsigned p = 0; p < 16; ++p) bits |= code[(p + add) & 7] << (16 + 3 * p);
        std::array<std::uint8_t, 8> bytes{};
        const std::uint64_t value = e0 | e1 << 8 | bits;
        for (std::size_t k = 0; k < 8; ++k) bytes[k] = static_cast<std::uint8_t>(value >> (8 * k));
        return bytes;
    };
    std::array<std::uint8_t, 16> out{};
    const auto first = block(138, 125, 0);
    const auto second = block(139, 126, 1);
    std::copy(first.begin(), first.end(), out.begin());
    std::copy(second.begin(), second.end(), out.begin() + 8);
    return out;
}

struct OpenSource {
    TempFile temp;
    void* file;
};

OpenSource file_with(const std::vector<std::uint8_t>& bytes) {
    TempFile temp;
    write_file(temp.path, bytes);
    void* handle = open(temp.path);
    return OpenSource{std::move(temp), handle};
}

void read_lane(Lanes& lanes, std::uint64_t id, std::uint64_t options, std::uint32_t size) {
    Request value = request(options, nullptr, 0, nullptr, 0, 0);
    value.destination_size = size;
    value.source_size = size;
    lanes.lanes.at(id).unsubmitted.push_back(Command(std::in_place_type<Request>, value));
}

std::vector<std::size_t> allocate_pages(std::size_t count) {
    std::vector<std::size_t> pages(count, 0);
    std::size_t given = count;
    CHECK(g6shim_XMemAllocatePhysicalPages(0, &given, pages.data()) == 1);
    CHECK(given == count);
    return pages;
}

}  // namespace

TEST(xcrdapi_request_layout_is_the_titles) {
    CHECK(sizeof(Request) == 0x48);
    CHECK(offsetof(Request, destination_size) == 0x18);
    CHECK(offsetof(Request, source) == 0x20);
    CHECK(offsetof(Request, source_size) == 0x30);
    CHECK(offsetof(Request, name) == 0x40);
    CHECK(offsetof(QueueDesc, unknown) == 0x18);
}

TEST(xcrdapi_raw_and_zlib_reads_from_a_file) {
    const std::vector<std::uint8_t> plain = pattern(300'000);
    const std::vector<std::uint8_t> packed = zlib_stored(plain);
    std::vector<std::uint8_t> contents = plain;
    contents.insert(contents.end(), packed.begin(), packed.end());
    OpenSource source = file_with(contents);
    void* queue = new_queue(0x80, 0, 0);
    void* array = status_array(1);

    std::vector<std::uint8_t> raw(1000, 0);
    std::vector<std::uint8_t> inflated(plain.size(), 0);
    enqueue(queue, request(0, raw.data(), raw.size(), source.file, 5000, 1000));
    enqueue(queue, request(kOptionZlib, inflated.data(), inflated.size(), source.file, plain.size(),
                           packed.size()));
    enqueue_status(queue, array, 0);
    CHECK(result(array, 0) == kPending);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    CHECK(std::equal(raw.begin(), raw.end(), plain.begin() + 5000));
    CHECK(inflated == plain);
    close_queue(queue);
}

TEST(xcrdapi_memory_sources_inflate_too) {
    const std::vector<std::uint8_t> plain = pattern(70'000);
    const std::vector<std::uint8_t> packed = zlib_stored(plain);
    void* queue = new_queue(0x80, 0, 1);
    void* array = status_array(1);
    std::vector<std::uint8_t> out(plain.size(), 0);
    const std::uint64_t options = kOptionZlib | kOptionMemorySource;
    enqueue(queue, request(options, out.data(), out.size(), packed.data(), 0, packed.size()));
    enqueue_status(queue, array, 0);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    CHECK(out == plain);
}

TEST(xcrdapi_statuses_complete_in_order_with_the_first_failure) {
    const std::vector<std::uint8_t> contents = pattern(4096);
    OpenSource source = file_with(contents);
    void* queue = new_queue(0x80, 0, 0);
    void* array = status_array(3);
    std::array<std::array<std::uint8_t, 16>, 5> buffers{};

    enqueue(queue, request(0, buffers[0].data(), 16, source.file, 4090, 16));
    enqueue(queue, request(kOptionZlib, buffers[1].data(), 16, source.file, 0, 16));
    enqueue_status(queue, array, 0);
    enqueue(queue, request(0, buffers[3].data(), 16, source.file, 0, 16));
    enqueue(queue, request(0, buffers[2].data(), 16, source.file, 100, 16));
    enqueue_status(queue, array, 1);
    submit(queue);
    with_test_tables();
    enqueue(queue, request(7u << 1, buffers[4].data(), 16, source.file, 0, 0));
    enqueue_status(queue, array, 2);
    submit(queue);

    CHECK(wait(array, 2) == kInvalidData);
    CHECK(result(array, 0) == kEndOfFile);
    CHECK(result(array, 1) == kSucceeded);
    CHECK(std::equal(buffers[2].begin(), buffers[2].end(), contents.begin() + 100));
    CHECK(std::equal(buffers[3].begin(), buffers[3].end(), contents.begin()));
}

TEST(xcrdapi_half_a_queue_submits_itself) {
    const std::vector<std::uint8_t> contents = pattern(64);
    OpenSource source = file_with(contents);
    void* queue = new_queue(4, 1, 0);
    void* array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    enqueue(queue, request(0, out.data(), out.size(), source.file, 8, 16));
    enqueue_status(queue, array, 0);
    CHECK(wait(array, 0) == kSucceeded);
    CHECK(std::equal(out.begin(), out.end(), contents.begin() + 8));
}

TEST(xcrdapi_priorities_share_by_bytes_about_tenfold) {
    Lanes lanes;
    lanes.lanes.emplace(0, Lane::make(0, 4096));
    lanes.lanes.emplace(1, Lane::make(1, 4096));
    lanes.lanes.emplace(2, Lane::make(2, 4096));
    for (unsigned round = 0; round < 400; ++round)
        for (std::uint64_t id = 0; id < 3; ++id) read_lane(lanes, id, 0, 4096);
    for (std::uint64_t id = 0; id < 3; ++id) lanes.submit(id);
    std::array<std::uint32_t, 3> started{};
    for (unsigned round = 0; round < 330; ++round) {
        const auto job = lanes.start();
        CHECK(job.has_value());
        if (!job) break;
        started[job->lane] += 1;
        lanes.finish(*job, kSucceeded);
    }
    CHECK(started[1] + started[2] == 300);
    CHECK(started[0] == 30);
    CHECK(started[1] > started[2] ? started[1] - started[2] <= 1 : started[2] - started[1] <= 1);
}

TEST(xcrdapi_staging_bounds_compressed_bytes_in_flight) {
    Lanes lanes;
    lanes.staging = 100;
    lanes.lanes.emplace(0, Lane::make(0, 64));
    read_lane(lanes, 0, kOptionZlib, 60);
    read_lane(lanes, 0, kOptionZlib, 60);
    read_lane(lanes, 0, 0, 1000);
    lanes.submit(0);
    const auto first = lanes.start();
    CHECK(first.has_value());
    CHECK(!lanes.start().has_value());
    if (!first) return;
    lanes.finish(*first, kSucceeded);
    const auto second = lanes.start();
    const auto raw = lanes.start();
    CHECK(second.has_value());
    CHECK(raw.has_value());
    if (!second || !raw) return;
    CHECK(second->staging == 60);
    CHECK(raw->staging == 0);
    read_lane(lanes, 0, kOptionZlib, 500);
    lanes.submit(0);
    CHECK(!lanes.start().has_value());
    lanes.finish(*second, kSucceeded);
    CHECK(lanes.start().has_value());
}

TEST(xcrdapi_a_status_waits_for_every_earlier_read) {
    Lanes lanes;
    lanes.lanes.emplace(0, Lane::make(0, 64));
    auto* array = static_cast<Instance<StatusArray>*>(status_array(2));
    const auto status = [array](std::uint32_t index) {
        add_ref_instance(array);
        return Command(std::in_place_type<Status>, array, index);
    };
    read_lane(lanes, 0, 0, 16);
    read_lane(lanes, 0, 0, 16);
    lanes.lanes.at(0).unsubmitted.push_back(status(0));
    read_lane(lanes, 0, 0, 16);
    lanes.lanes.at(0).unsubmitted.push_back(status(1));
    for (auto& entry : array->state.entries) entry.store(kPending, std::memory_order_relaxed);
    lanes.submit(0);
    std::vector<Job> jobs;
    for (unsigned k = 0; k < 3; ++k) {
        const auto job = lanes.start();
        CHECK(job.has_value());
        if (!job) return;
        jobs.push_back(*job);
    }
    lanes.finish(jobs[2], kSucceeded);
    lanes.finish(jobs[0], kSucceeded);
    CHECK(result(array, 0) == kPending);
    CHECK(result(array, 1) == kPending);
    lanes.finish(jobs[1], kInvalidData);
    CHECK(result(array, 0) == kInvalidData);
    CHECK(result(array, 1) == kSucceeded);
}

TEST(xcrdapi_zlib_then_bcpack) {
    with_test_tables();
    const std::vector<std::uint8_t> plain(kBc4Stream.begin(), kBc4Stream.end());
    const std::vector<std::uint8_t> packed = zlib_stored(plain);
    OpenSource source = file_with(packed);
    void* queue = new_queue(0x80, 0, 0);
    void* array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    const std::uint64_t options = kOptionZlib | (4u << 1);
    Request value = request(options, out.data(), out.size(), source.file, 0, packed.size());
    value.intermediate_size = static_cast<std::uint32_t>(kBc4Stream.size());
    enqueue(queue, value);
    enqueue_status(queue, array, 0);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    CHECK(out == bc4_blocks());
}

TEST(xcrdapi_bcpack_requests_unpack_into_the_destination) {
    with_test_tables();
    std::vector<std::uint8_t> contents = pattern(100);
    contents.insert(contents.end(), kBc4Stream.begin(), kBc4Stream.end());
    OpenSource source = file_with(contents);
    void* queue = new_queue(0x80, 0, 0);
    void* array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    enqueue(queue,
            request(4u << 1, out.data(), out.size(), source.file, 100, kBc4Stream.size()));
    enqueue_status(queue, array, 0);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    CHECK(out == bc4_blocks());
}

TEST(xcrdapi_a_closed_queue_completes_nothing) {
    const std::vector<std::uint8_t> contents = pattern(64);
    OpenSource source = file_with(contents);
    void* queue = new_queue(0x80, 0, 0);
    void* array = status_array(1);
    std::array<std::uint8_t, 16> out{};
    enqueue(queue, request(0, out.data(), out.size(), source.file, 0, 16));
    enqueue_status(queue, array, 0);
    close_queue(queue);
    submit(queue);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(result(array, 0) == kPending);
    CHECK(std::all_of(out.begin(), out.end(), [](std::uint8_t byte) { return byte == 0; }));
}

TEST(xcrdapi_physical_page_destinations) {
    std::array<std::size_t, 2> pages{};
    std::size_t count = 2;
    CHECK(g6shim_XMemAllocatePhysicalPages(0, &count, pages.data()) == 1);
    void* mapped = g6shim_XMemVirtualAlloc(nullptr, 2 * kDestinationPageBytes,
                                           MEM_RESERVE | g6shim::xmem::kPages64K, 0, PAGE_READWRITE);
    CHECK(mapped != nullptr);
    CHECK(g6shim_XMemMapPhysicalPages(mapped, 2, pages.data()) == mapped);

    const std::vector<std::uint8_t> plain = pattern(100);
    void* queue = new_queue(0x80, 0, 1);
    void* array = status_array(1);
    Request value = request(kOptionPageDestination | kOptionMemorySource, pages.data(), plain.size(),
                            plain.data(), 0, plain.size());
    value.page_offset = kDestinationPageBytes - 40;
    enqueue(queue, value);
    enqueue_status(queue, array, 0);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    const auto* seen = static_cast<const std::uint8_t*>(mapped) + kDestinationPageBytes - 40;
    CHECK(std::equal(plain.begin(), plain.end(), seen));
}

TEST(xcrdapi_zlib_into_pages_in_place_and_across_allocations) {
    const std::vector<std::size_t> a = allocate_pages(3);
    const std::vector<std::size_t> b = allocate_pages(1);
    const std::vector<std::uint8_t> plain = pattern(2 * kDestinationPageBytes + 100);
    const std::vector<std::uint8_t> packed = zlib_stored(plain);
    std::vector<std::vector<std::size_t>> destinations{a, {a[0], a[1], b[0]}};
    void* queue = new_queue(0x80, 0, 1);
    void* array = status_array(1);
    for (const auto& pages : destinations) {
        Request value = request(kOptionZlib | kOptionPageDestination | kOptionMemorySource,
                                const_cast<std::size_t*>(pages.data()), plain.size(), packed.data(),
                                0, packed.size());
        value.page_offset = 50;
        enqueue(queue, value);
    }
    enqueue_status(queue, array, 0);
    submit(queue);
    CHECK(wait(array, 0) == kSucceeded);
    for (const auto& pages : destinations) {
        void* mapped = g6shim_XMemVirtualAlloc(nullptr, 3 * kDestinationPageBytes,
                                               MEM_RESERVE | g6shim::xmem::kPages64K, 0,
                                               PAGE_READWRITE);
        CHECK(mapped != nullptr);
        CHECK(g6shim_XMemMapPhysicalPages(mapped, 3, pages.data()) == mapped);
        const auto* seen = static_cast<const std::uint8_t*>(mapped);
        CHECK(std::all_of(seen, seen + 50, [](std::uint8_t byte) { return byte == 0; }));
        CHECK(std::equal(plain.begin(), plain.end(), seen + 50));
        CHECK(g6shim_XMemMapPhysicalPages(mapped, 3, nullptr) == mapped);
        VirtualFree(mapped, 0, MEM_RELEASE);
    }
    CHECK(g6shim_XMemFreePhysicalPages(3, a.data()) == 1);
    CHECK(g6shim_XMemFreePhysicalPages(1, b.data()) == 1);
}
