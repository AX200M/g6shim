// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "sample_pe.hpp"

#include "bcpack.hpp"
#include "dstorage.hpp"
#include "g6shim/pe.hpp"
#include "objects.hpp"
#include "xma.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

extern "C" HRESULT WINAPI g6shim_DStorageGetFactory(const g6shim::xcrdapi::Guid* iid, void** out);
extern "C" BOOL WINAPI g6shim_XMemAllocatePhysicalPages(ULONGLONG flags, SIZE_T* count, SIZE_T* pages);
extern "C" BOOL WINAPI g6shim_XMemFreePhysicalPages(SIZE_T count, const SIZE_T* pages);

namespace {

using g6shim::pe::ParseError;
using namespace g6shim::xcrdapi;
using namespace g6shim::acphal::xma;

constexpr std::uint32_t kPeSeed = 0x5A1D'0F11u;
constexpr std::uint32_t kStorageSeed = 0x5A1D'0F22u;
constexpr std::uint32_t kXmaSeed = 0x5A1D'0F33u;
constexpr int kPeRounds = 2500;
constexpr int kStorageRounds = 2000;
constexpr int kXmaRounds = 2000;
constexpr std::size_t kStorageBytes = 4096;

constexpr Guid kFactoryInterface{0x62959d11u, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

std::vector<std::uint8_t> random_bytes(std::mt19937& rng, std::size_t size) {
    std::vector<std::uint8_t> out(size);
    for (std::uint8_t& byte : out) byte = static_cast<std::uint8_t>(rng());
    return out;
}

void mutate(std::mt19937& rng, std::vector<std::uint8_t>& data) {
    const auto edit = [&] {
        if (data.empty()) return;
        const std::size_t at = static_cast<std::size_t>(rng()) % data.size();
        switch (rng() % 6) {
            case 0: data[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
            case 1: data[at] = static_cast<std::uint8_t>(rng()); break;
            case 2: data.resize(at); break;
            case 3:
                data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::uint8_t>(rng()));
                break;
            case 4: {
                const std::size_t n =
                    1 + static_cast<std::size_t>(rng()) % std::min<std::size_t>(data.size() - at, 24);
                data.insert(data.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                            data.begin() + static_cast<std::ptrdiff_t>(at + n));
                break;
            }
            default: {
                if (data.size() < 2) break;
                const std::size_t from = static_cast<std::size_t>(rng()) % data.size();
                const std::size_t n =
                    1 + static_cast<std::size_t>(rng()) % std::min(data.size() - from, data.size() - at);
                const std::vector<std::uint8_t> piece(data.begin() + static_cast<std::ptrdiff_t>(from),
                                                      data.begin() + static_cast<std::ptrdiff_t>(from + n));
                std::copy(piece.begin(), piece.end(), data.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            }
        }
    };
    const unsigned edits = 1 + rng() % 6;
    for (unsigned index = 0; index < edits; ++index) edit();
    if (rng() % 4 == 0) {
        const std::vector<std::uint8_t> tail = random_bytes(rng, rng() % 32);
        data.insert(data.end(), tail.begin(), tail.end());
    }
}

void cap_size(std::vector<std::uint8_t>& data, std::size_t limit) {
    if (data.size() > limit) data.resize(limit);
}

bool parse_accepts(const std::vector<std::uint8_t>& bytes) {
    try {
        (void)g6shim::pe::parse(bytes);
        return true;
    } catch (const ParseError&) {
        return true;
    } catch (...) {
        return false;
    }
}

void poke_u16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    if (at + sizeof value > bytes.size()) return;
    std::memcpy(bytes.data() + at, &value, sizeof value);
}

void poke_u32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    if (at + sizeof value > bytes.size()) return;
    std::memcpy(bytes.data() + at, &value, sizeof value);
}

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
};

void* factory() {
    void* object = nullptr;
    CHECK(g6shim_DStorageGetFactory(&kFactoryInterface, &object) == kSucceeded);
    return object;
}

void* open_file(const std::wstring& path) {
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
    CHECK(fn(factory(), capacity, "fuzz", nullptr, &array) == kSucceeded);
    return array;
}

void* new_queue() {
    using Create = HRESULT(WINAPI*)(void*, const QueueDesc*, const Guid*, void**);
    const Create fn = method<Create>(factory(), 0x50);
    const QueueDesc desc{1, 8, 0, "fuzz", 1};
    void* queue = nullptr;
    CHECK(fn(factory(), &desc, nullptr, &queue) == kSucceeded);
    return queue;
}

void enqueue(void* queue, const Request& value) {
    using Enqueue = void(WINAPI*)(void*, const Request*);
    method<Enqueue>(queue, 0x18)(queue, &value);
}

void enqueue_status(void* queue, void* array, std::uint32_t index) {
    using Enqueue = void(WINAPI*)(void*, void*, std::uint32_t);
    method<Enqueue>(queue, 0x20)(queue, array, index);
}

void submit(void* queue) {
    using Submit = void(WINAPI*)(void*);
    method<Submit>(queue, 0x30)(queue);
}

HRESULT wait_done(void* array, std::uint32_t index) {
    using Complete = BOOL(WINAPI*)(void*, std::uint32_t);
    using Result = HRESULT(WINAPI*)(void*, std::uint32_t);
    const Complete complete = method<Complete>(array, 0x18);
    const Result result = method<Result>(array, 0x20);
    const auto started = std::chrono::steady_clock::now();
    while (complete(array, index) == 0) {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
            std::fputs("dstorage fuzz: request timed out\n", stderr);
            std::abort();
        }
    }
    return result(array, index);
}

HRESULT run_request(void* queue, void* array, const Request& value) {
    enqueue(queue, value);
    enqueue_status(queue, array, 0);
    submit(queue);
    return wait_done(array, 0);
}

struct Tail {
    std::uint8_t* reserve = nullptr;
    std::uint8_t* data = nullptr;
    std::size_t size = 0;

    explicit Tail(std::size_t n) : size(n) {
        constexpr std::size_t kPage = 4096;
        const std::size_t span = n == 0 ? 0 : (n + kPage - 1) & ~(kPage - 1);
        reserve = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, span + kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(reserve != nullptr);
        data = reserve + span - n;
        DWORD old = 0;
        CHECK(VirtualProtect(reserve + span, kPage, PAGE_NOACCESS, &old) != 0);
    }
    ~Tail() {
        if (reserve != nullptr) VirtualFree(reserve, 0, MEM_RELEASE);
    }
    Tail(const Tail&) = delete;
    Tail& operator=(const Tail&) = delete;
};

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& plain) {
    std::vector<std::uint8_t> out{0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(plain.size());
    const auto complement = static_cast<std::uint16_t>(~length);
    out.push_back(static_cast<std::uint8_t>(length));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.push_back(static_cast<std::uint8_t>(complement));
    out.push_back(static_cast<std::uint8_t>(complement >> 8));
    out.insert(out.end(), plain.begin(), plain.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : plain) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(adler >> shift));
    return out;
}

std::vector<std::uint8_t> test_tables() {
    std::vector<std::uint8_t> bytes;
    for (std::size_t m = 0; m < kBcpackModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < kBcpackBuckets; ++b) {
            const std::uint16_t f = b < g + 2 ? static_cast<std::uint16_t>(1u << (15 - g)) : std::uint16_t{0};
            bytes.push_back(static_cast<std::uint8_t>(f));
            bytes.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v & 0x77));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>((v & 7) | ((v >> 3) << 4)));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned code = 0; code < 512; ++code) {
        const std::uint16_t ctrl = code == 0 ? 0x46 : 0;
        bytes.push_back(static_cast<std::uint8_t>(ctrl));
        bytes.push_back(static_cast<std::uint8_t>(ctrl >> 8));
    }
    bytes.resize(kBcpackTablesSize, 0);
    return bytes;
}

void put_bits(std::vector<std::uint8_t>& data, std::size_t pos, std::uint32_t count, std::uint32_t value) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = pos + i;
        if (at / 8 >= data.size()) return;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (7 - at % 8));
        if (((value >> (count - 1 - i)) & 1u) != 0)
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] | mask);
        else
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] & static_cast<std::uint8_t>(~mask));
    }
}

std::uint32_t packet_header(std::uint32_t frames, std::uint32_t first, std::uint32_t skip) {
    return (frames << 26) | ((first & 0x7fffu) << 11) | (skip & 0xffu);
}

void walk_packets(const std::vector<std::uint8_t>& data) {
    const std::size_t packets = data.empty() ? 1 : data.size() / kPacketBytes + 1;
    for (std::size_t index = 0; index < packets; ++index) (void)packet(data, index);
    (void)packet(data, SIZE_MAX / kPacketBytes + 1);
    const std::size_t packet_bits = kPacketBytes * 8;
    const std::size_t starts[] = {0, 1, 8, 31, 32, 33, packet_bits - 1, packet_bits, packet_bits + 32};
    for (const std::size_t start : starts) {
        const std::optional<Frame> frame = read_frame(data, start);
        if (!frame || !frame->next) continue;
        (void)read_frame(data, *frame->next);
    }
}

long long milliseconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
}

TEST(pe_fuzz_mutated_images) {
    const auto started = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> sample = g6shim::test::build_sample_pe();
    std::mt19937 rng(kPeSeed);
    bool accepted = true;
    for (int round = 0; round < kPeRounds; ++round) {
        std::vector<std::uint8_t> bytes = (rng() % 5 == 0) ? random_bytes(rng, rng() % 512) : sample;
        mutate(rng, bytes);
        cap_size(bytes, 8192);
        if (!bytes.empty() && rng() % 3 == 0) {
            const std::size_t coff = 0x44;
            switch (rng() % 8) {
                case 0: poke_u32(bytes, 0x3c, rng()); break;
                case 1: poke_u16(bytes, coff + 2, static_cast<std::uint16_t>(rng())); break;
                case 2: poke_u16(bytes, coff + 16, static_cast<std::uint16_t>(rng())); break;
                case 3: poke_u16(bytes, g6shim::test::kSampleOptional, static_cast<std::uint16_t>(rng())); break;
                case 4: poke_u32(bytes, g6shim::test::kSampleOptional + 56, rng()); break;
                case 5: poke_u32(bytes, g6shim::test::kSampleOptional + 60, rng()); break;
                case 6: poke_u32(bytes, g6shim::test::kSampleDirectories + (rng() % 16) * 8, rng()); break;
                default: poke_u32(bytes, g6shim::test::kSampleSections + 12 + (rng() % 4) * 4, rng()); break;
            }
        }
        if (!parse_accepts(bytes)) {
            std::fprintf(stderr, "pe fuzz: unexpected exception at round %d\n", round);
            accepted = false;
            break;
        }
    }
    std::vector<std::uint8_t> hostile = sample;
    poke_u16(hostile, g6shim::test::kSampleCoff + 2, 0xffff);
    accepted = accepted && parse_accepts(hostile);
    poke_u32(hostile, g6shim::test::kSampleDirectories, 0x1000);
    poke_u32(hostile, g6shim::test::kSampleDirectories + 4, 0xffffffffu);
    poke_u32(hostile, g6shim::test::kSampleDirectories + 8, 0x1000);
    poke_u32(hostile, g6shim::test::kSampleDirectories + 12, 0xffffffffu);
    poke_u32(hostile, g6shim::test::kSampleDirectories + 13 * 8, 0x1000);
    poke_u32(hostile, g6shim::test::kSampleDirectories + 13 * 8 + 4, 0xffffffffu);
    accepted = accepted && parse_accepts(hostile);
    std::vector<std::uint8_t> wrapped = sample;
    poke_u32(wrapped, g6shim::test::kSampleSections + 20, 0xfffffff0u);
    poke_u32(wrapped, g6shim::test::kSampleSections + 8, 0xffffffffu);
    poke_u32(wrapped, g6shim::test::kSampleSections + 16, 0xffffffffu);
    accepted = accepted && parse_accepts(wrapped);
    accepted = accepted && parse_accepts({});
    accepted = accepted && parse_accepts(std::vector<std::uint8_t>{0x4d, 0x5a});
    const auto probed = g6shim::pe::parse(sample);
    const std::size_t export_off = static_cast<std::size_t>(probed.offset_of(probed.directories[0].rva));
    std::vector<std::uint8_t> exports = sample;
    poke_u32(exports, export_off + 20, 0xffffffffu);
    poke_u32(exports, export_off + 24, 0xffffffffu);
    accepted = accepted && parse_accepts(exports);
    poke_u32(exports, export_off + 20, 1u << 20);
    poke_u32(exports, export_off + 24, 1u << 20);
    accepted = accepted && parse_accepts(exports);
    CHECK(accepted);
    const auto image = g6shim::pe::parse(sample);
    CHECK(image.machine == 0x8664);
    CHECK(image.imports.size() == 3);
    std::printf("pe_fuzz %lld ms seed 0x%08x rounds %d\n", milliseconds_since(started), kPeSeed, kPeRounds);
}

TEST(dstorage_fuzz_mutated_requests) {
    const auto started = std::chrono::steady_clock::now();
    use_bcpack_tables(test_tables());
    TempFile temp;
    std::mt19937 file_rng(kStorageSeed);
    const std::vector<std::uint8_t> file_bytes = random_bytes(file_rng, 256);
    const HANDLE raw =
        CreateFileW(temp.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(raw != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(raw, file_bytes.data(), static_cast<DWORD>(file_bytes.size()), &written, nullptr) != 0);
    CloseHandle(raw);
    void* file = open_file(temp.path);
    void* queue = new_queue();
    void* statuses = status_array(1);
    std::array<std::size_t, 2> pages{};
    std::size_t page_count = 2;
    CHECK(g6shim_XMemAllocatePhysicalPages(0, &page_count, pages.data()) == 1);
    CHECK(page_count == 2);

    std::mt19937 rng(kStorageSeed);
    char name[32] = "fuzz";
    const std::vector<std::uint8_t> plain{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> packed = zlib_stored(plain);
    Tail good_source(packed.size());
    Tail good_dest(plain.size());
    std::memcpy(good_source.data, packed.data(), packed.size());
    const Request good{kOptionZlib | kOptionMemorySource,
                       good_dest.data,
                       0,
                       static_cast<std::uint32_t>(plain.size()),
                       good_source.data,
                       0,
                       static_cast<std::uint32_t>(packed.size()),
                       0,
                       0,
                       name};
    CHECK(run_request(queue, statuses, good) == kSucceeded);
    CHECK(std::memcmp(good_dest.data, plain.data(), plain.size()) == 0);

    for (int round = 0; round < kStorageRounds; ++round) {
        const std::uint32_t source_size = static_cast<std::uint32_t>(1 + rng() % kStorageBytes);
        const std::uint32_t dest_size = static_cast<std::uint32_t>(1 + rng() % kStorageBytes);
        Tail source(source_size);
        Tail dest(dest_size);
        for (std::size_t i = 0; i < source_size; ++i) source.data[i] = static_cast<std::uint8_t>(rng());
        if (rng() % 5 == 0 && source_size >= packed.size()) std::memcpy(source.data, packed.data(), packed.size());
        for (std::size_t i = 0; i + 1 < sizeof name; ++i) {
            if (rng() % 4 == 0) name[i] = static_cast<char>(rng() % 95 + 32);
        }
        name[sizeof name - 1] = 0;

        std::uint64_t options = kOptionMemorySource;
        const unsigned mode = rng() % 8;
        if (mode == 0 || mode == 1 || mode == 4) options |= kOptionZlib;
        if (mode == 2 || mode == 3 || mode == 4) options |= static_cast<std::uint64_t>(1 + rng() % 15) << 1;
        if (mode == 5) options |= kOptionPageDestination;
        if (mode == 6) options = (rng() & ~(kOptionPageDestination | kOptionMemorySource)) | kOptionMemorySource;
        if (mode == 7) options = (options | (rng() & ~kOptionPageDestination)) & ~kOptionMemorySource;
        const bool memory_source = (options & kOptionMemorySource) != 0;
        const bool page_dest = (options & kOptionPageDestination) != 0;
        std::uint32_t used_source = memory_source ? source_size : static_cast<std::uint32_t>(rng() % 512);
        std::uint32_t used_dest = dest_size;
        if (page_dest) {
            used_dest = static_cast<std::uint32_t>(rng() % 4096);
            if (used_source > used_dest) used_source = used_dest;
        }
        const void* source_ptr = memory_source ? static_cast<const void*>(source.data) : static_cast<const void*>(file);
        void* dest_ptr = page_dest ? static_cast<void*>(pages.data()) : static_cast<void*>(dest.data);
        if (rng() % 11 == 0) source_ptr = nullptr;
        if (rng() % 13 == 0) dest_ptr = nullptr;
        const Request value{options,
                            dest_ptr,
                            rng(),
                            used_dest,
                            source_ptr,
                            (static_cast<std::uint64_t>(rng()) << 32) | rng(),
                            used_source,
                            rng() % 8192,
                            rng(),
                            rng() % 6 == 0 ? nullptr : name};
        (void)run_request(queue, statuses, value);
    }

    Tail exact_source(8);
    Tail exact_dest(4);
    std::memcpy(exact_source.data, "01234567", 8);
    const Request oversized{kOptionMemorySource, exact_dest.data, 0, 4, exact_source.data, 0, 8, 0, 0, nullptr};
    CHECK(run_request(queue, statuses, oversized) == kInvalidArgument);
    const Request exact{kOptionMemorySource, exact_dest.data, 0, 4, exact_source.data, 0, 4, 0, 0, nullptr};
    CHECK(run_request(queue, statuses, exact) == kSucceeded);
    CHECK(std::memcmp(exact_dest.data, "0123", 4) == 0);
    using Release = std::uint32_t(WINAPI*)(void*);
    method<Release>(file, 0x10)(file);
    CHECK(g6shim_XMemFreePhysicalPages(pages.size(), pages.data()) == 1);
    std::printf("dstorage_fuzz %lld ms seed 0x%08x rounds %d\n", milliseconds_since(started), kStorageSeed,
                kStorageRounds);
}

TEST(xma_fuzz_mutated_packets) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> canonical(2 * kPacketBytes, 0xff);
    put_bits(canonical, 0, 32, packet_header(2, 0, 0));
    put_bits(canonical, kPacketBytes * 8, 32, packet_header(1, 48, 0));
    put_bits(canonical, 32, 15, 100);
    put_bits(canonical, 132, 15, 200);
    const std::optional<Frame> known = read_frame(canonical, 32);
    CHECK(known.has_value());
    CHECK(known->bits == 100);
    CHECK(known->next.has_value() && *known->next == 132);

    std::vector<std::uint8_t> hostile(kPacketBytes, 0);
    put_bits(hostile, 0, 32, packet_header(63, 0x7fff, 255));
    put_bits(hostile, 32, 15, 0x7ffe);
    walk_packets(hostile);
    hostile.resize(kPacketBytes * 2 + 3, 0xa5);
    put_bits(hostile, 0, 32, packet_header(2, 0, 1));
    put_bits(hostile, 32, 15, 16000);
    walk_packets(hostile);
    walk_packets({});
    walk_packets(std::vector<std::uint8_t>(3, 0));
    walk_packets(std::vector<std::uint8_t>(kPacketBytes + 4, 0));

    Decoder decoder(2, 48000);
    std::mt19937 rng(kXmaSeed);
    for (int round = 0; round < kXmaRounds; ++round) {
        std::vector<std::uint8_t> data = (rng() % 4 == 0) ? random_bytes(rng, rng() % (kPacketBytes + 64)) : canonical;
        mutate(rng, data);
        cap_size(data, kPacketBytes * 3);
        if (data.size() >= 4 && rng() % 2 == 0) {
            const std::uint32_t header = packet_header(rng() % 64, rng() & 0x7fff, rng() & 0xff);
            data[0] = static_cast<std::uint8_t>(header >> 24);
            data[1] = static_cast<std::uint8_t>(header >> 16);
            data[2] = static_cast<std::uint8_t>(header >> 8);
            data[3] = static_cast<std::uint8_t>(header);
        }
        (void)packet(data, 0);
        (void)packet(data, data.size() / kPacketBytes);
        const std::size_t origin = data.size() < 5 ? 0 : 32;
        if (const std::optional<Frame> first = read_frame(data, origin)) {
            if (first->next) (void)read_frame(data, *first->next);
            std::vector<std::int16_t> pcm;
            (void)decoder.decode(first->data, first->bits, pcm);
        }
        const std::size_t start = data.empty() ? 0 : static_cast<std::size_t>(rng()) % (data.size() * 8 + 64);
        if (const std::optional<Frame> frame = read_frame(data, start)) {
            if (frame->next) (void)read_frame(data, *frame->next);
            if (rng() % 4 == 0) {
                std::vector<std::int16_t> pcm;
                (void)decoder.decode(frame->data, frame->bits, pcm);
            }
        }
        if (rng() % 3 == 0) decoder.reset();
    }
    const std::optional<Frame> again = read_frame(canonical, 32);
    CHECK(again.has_value() && again->bits == 100);
    std::printf("xma_fuzz %lld ms seed 0x%08x rounds %d\n", milliseconds_since(started), kXmaSeed, kXmaRounds);
}

}  // namespace
