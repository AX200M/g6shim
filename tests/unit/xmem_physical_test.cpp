// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "physical.hpp"
#include "virtual_memory.hpp"

#include <g6shim/title_memory.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

using namespace g6shim::xmem;

extern "C" void* WINAPI g6shim_XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type, ULONGLONG flags,
                                                DWORD protect);
extern "C" BOOL WINAPI g6shim_XMemAllocatePhysicalPages(ULONGLONG flags, SIZE_T* count, SIZE_T* pages);
extern "C" BOOL WINAPI g6shim_XMemFreePhysicalPages(SIZE_T count, const SIZE_T* pages);
extern "C" void* WINAPI g6shim_XMemMapPhysicalPages(void* address, SIZE_T count, const SIZE_T* pages);
extern "C" BOOL WINAPI g6shim_XMemMapPhysicalPagesScatter(void* const* addresses, SIZE_T count,
                                                          const SIZE_T* pages);

namespace {

std::map<const void*, int> g_remaps;

void record_remap(const void* at, std::size_t) {
    ++g_remaps[at];
    std::fprintf(stderr, "REMAP %p\n", at);
}

std::vector<std::size_t> allocate_pages(std::size_t n) {
    std::vector<std::size_t> pages(n, 0);
    std::size_t given = n;
    CHECK(g6shim_XMemAllocatePhysicalPages(0, &given, pages.data()) == 1);
    CHECK(given == n);
    return pages;
}

DWORD query_state(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(address, &info, sizeof info);
    return info.State;
}

std::vector<unsigned char> page_bytes(std::size_t page) {
    std::vector<unsigned char> bytes(kPageBytes, 0);
    PageWindow window;
    const std::size_t one[1] = {page};
    CHECK(open_page_window(one, 1, 0, kPageBytes, &window));
    std::memcpy(bytes.data(), window.bytes().data(), kPageBytes);
    return bytes;
}

bool all_zero(const std::vector<unsigned char>& bytes, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

}  // namespace

TEST(xmem_physical_pages_map_alias_and_unmap) {
    std::size_t pages[3] = {};
    std::size_t n = 3;
    CHECK(g6shim_XMemAllocatePhysicalPages(0, &n, pages) == 1);
    CHECK(n == 3);
    for (std::size_t p : pages) CHECK((p >> 4 & 0x1fffff) << 4 == p);

    const auto reserve = [] {
        return g6shim_XMemVirtualAlloc(nullptr, 3 * kPageBytes, MEM_RESERVE | kPages64K, 0, PAGE_READWRITE);
    };
    void* a = reserve();
    void* b = reserve();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    const auto at = [](void* base, std::size_t page) {
        return static_cast<unsigned char*>(base) + page * kPageBytes;
    };

    CHECK(g6shim_XMemMapPhysicalPages(a, 3, pages) == a);
    CHECK(g6shim_XMemMapPhysicalPages(at(b, 1), 1, pages + 2) == at(b, 1));
    *at(a, 2) = 7;
    CHECK(*at(b, 1) == 7);
    *static_cast<unsigned char*>(a) = 1;

    const unsigned char nine = 9;
    CHECK(write_physical(pages + 2, 1, 1, &nine, 1));
    CHECK(at(b, 1)[1] == 9);

    CHECK(g6shim_XMemMapPhysicalPages(at(a, 1), 1, nullptr) == at(a, 1));
    CHECK(query_state(at(a, 1)) == MEM_RESERVE);
    CHECK(query_state(a) == MEM_COMMIT);
    CHECK(query_state(at(a, 2)) == MEM_COMMIT);
    CHECK(*at(a, 2) == 7);

    void* addresses[1] = {at(a, 1)};
    CHECK(g6shim_XMemMapPhysicalPagesScatter(addresses, 1, pages) == 1);
    CHECK(*at(a, 1) == 1);
    const std::size_t no_page[1] = {0};
    CHECK(g6shim_XMemMapPhysicalPagesScatter(addresses, 1, no_page) == 1);
    CHECK(query_state(at(a, 1)) == MEM_RESERVE);

    CHECK(map_alias(reinterpret_cast<std::uintptr_t>(b), reinterpret_cast<std::uintptr_t>(at(a, 2)), kPageBytes));
    CHECK(*static_cast<unsigned char*>(b) == 7);
    CHECK(map_alias(reinterpret_cast<std::uintptr_t>(b), 0, kPageBytes));
    CHECK(query_state(b) == MEM_RESERVE);
    CHECK(!map_alias(reinterpret_cast<std::uintptr_t>(b), reinterpret_cast<std::uintptr_t>(at(a, 1)), kPageBytes));

    CHECK(g6shim_XMemFreePhysicalPages(3, pages) == 1);
    VirtualFree(a, 0, MEM_RELEASE);
    VirtualFree(b, 0, MEM_RELEASE);
}

TEST(xmem_finds_a_stretch_of_one_section_across_views) {
    const std::vector<std::size_t> pages = allocate_pages(4);
    void* a = g6shim_XMemVirtualAlloc(nullptr, 4 * kPageBytes, MEM_RESERVE | kPages64K, 0, PAGE_READWRITE);
    CHECK(a != nullptr);
    if (a == nullptr) return;
    const auto page_at = [&](std::size_t n) { return static_cast<unsigned char*>(a) + n * kPageBytes; };
    CHECK(g6shim_XMemMapPhysicalPages(a, 2, pages.data()) == a);
    CHECK(g6shim_XMemMapPhysicalPages(page_at(2), 2, pages.data() + 2) == page_at(2));
    HANDLE section = nullptr;
    std::uint64_t offset = 0;
    const auto base = reinterpret_cast<std::size_t>(a);
    CHECK(find_stretch(base + kPageBytes + 16, 2 * kPageBytes, &section, &offset));
    CHECK(section != nullptr);
    CHECK(offset == kPageBytes + 16);
    const std::size_t swapped[2] = {pages[3], pages[2]};
    CHECK(g6shim_XMemMapPhysicalPages(page_at(2), 2, swapped) == page_at(2));
    CHECK(!find_stretch(base, 4 * kPageBytes, &section, &offset));
    CHECK(g6shim_XMemMapPhysicalPages(a, 4, nullptr) == a);
    CHECK(!find_stretch(base, kPageBytes, &section, &offset));
    CHECK(g6shim_XMemFreePhysicalPages(4, pages.data()) == 1);
    VirtualFree(a, 0, MEM_RELEASE);
}

TEST(xmem_mapping_no_pages_changes_nothing) {
    const std::vector<std::size_t> pages = allocate_pages(2);
    void* a = g6shim_XMemVirtualAlloc(nullptr, 2 * kPageBytes, MEM_RESERVE | kPages64K, 0, PAGE_READWRITE);
    CHECK(a != nullptr);
    CHECK(g6shim_XMemMapPhysicalPages(a, 2, pages.data()) == a);
    *static_cast<unsigned char*>(a) = 5;
    unsigned char* second = static_cast<unsigned char*>(a) + kPageBytes;

    for (void* where : {a, static_cast<void*>(second), static_cast<void*>(second + kPageBytes)}) {
        CHECK(g6shim_XMemMapPhysicalPages(where, 0, pages.data()) == where);
        CHECK(g6shim_XMemMapPhysicalPages(where, 0, nullptr) == where);
        CHECK(map_alias(reinterpret_cast<std::uintptr_t>(where), reinterpret_cast<std::uintptr_t>(a), 0));
        CHECK(map_alias(reinterpret_cast<std::uintptr_t>(where), 0, 0));
    }

    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(a, &info, sizeof info) != 0);
    CHECK(info.State == MEM_COMMIT);
    CHECK(info.RegionSize == 2 * kPageBytes);
    CHECK(VirtualQuery(second, &info, sizeof info) != 0);
    CHECK(info.State == MEM_COMMIT);
    CHECK(*static_cast<unsigned char*>(a) == 5);

    CHECK(g6shim_XMemMapPhysicalPages(second, 1, nullptr) == second);
    CHECK(query_state(second) == MEM_RESERVE);
    CHECK(query_state(a) == MEM_COMMIT);
    CHECK(g6shim_XMemMapPhysicalPages(a, 1, nullptr) == a);
    CHECK(g6shim_XMemFreePhysicalPages(2, pages.data()) == 1);
    VirtualFree(a, 0, MEM_RELEASE);
}

TEST(xmem_runs_group_consecutive_pages_of_one_allocation) {
    const std::vector<std::size_t> a = allocate_pages(3);
    const std::vector<std::size_t> b = allocate_pages(1);
    const auto lengths = [](const std::vector<std::size_t>& pages, std::size_t offset, std::size_t bytes) {
        std::vector<PageRun> runs;
        page_runs(pages.data(), pages.size(), offset, bytes, &runs);
        std::vector<std::size_t> out;
        for (const PageRun& run : runs) out.push_back(run.pages);
        return out;
    };
    using Pages = std::vector<std::size_t>;
    CHECK(lengths(a, 0, 3 * kPageBytes) == Pages{3});
    CHECK(lengths(a, kPageBytes + 5, 10) == Pages{1});
    CHECK(lengths(a, kPageBytes - 1, 2) == Pages{2});
    CHECK(lengths(a, 0, 0).empty());
    CHECK((lengths(Pages{a[0], a[2], a[1]}, 0, 3 * kPageBytes) == Pages{1, 1, 1}));
    CHECK((lengths(Pages{a[1], a[2], b[0]}, 0, 3 * kPageBytes) == Pages{2, 1}));
    CHECK((lengths(Pages{b[0], a[1]}, 0, 2 * kPageBytes) == Pages{1, 1}));
    std::vector<PageRun> runs;
    CHECK(!page_runs(a.data(), a.size(), 0, 3 * kPageBytes + 1, &runs));
    const std::size_t unknown[2] = {a[0], 0x10};
    CHECK(!page_runs(unknown, 2, 0, 2 * kPageBytes, &runs));
    CHECK(!page_runs(a.data(), a.size(), SIZE_MAX, 2, &runs));
    CHECK(g6shim_XMemFreePhysicalPages(3, a.data()) == 1);
    CHECK(g6shim_XMemFreePhysicalPages(1, b.data()) == 1);
}

TEST(xmem_writes_cross_runs_and_allocations) {
    const std::vector<std::size_t> a = allocate_pages(2);
    const std::vector<std::size_t> b = allocate_pages(1);
    const std::size_t pages[3] = {a[0], a[1], b[0]};
    std::vector<unsigned char> data(2 * kPageBytes + 64);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 7 % 251);
    const std::size_t offset = kPageBytes - 32;
    CHECK(!write_physical(pages, 3, offset, data.data(), data.size()));
    CHECK(write_physical(pages, 3, offset, data.data(), 2 * kPageBytes));
    std::vector<unsigned char> seen;
    for (std::size_t p : pages) {
        const std::vector<unsigned char> one = page_bytes(p);
        seen.insert(seen.end(), one.begin(), one.end());
    }
    CHECK(all_zero(seen, 0, offset));
    CHECK(std::memcmp(seen.data() + offset, data.data(), 2 * kPageBytes) == 0);
    CHECK(all_zero(seen, offset + 2 * kPageBytes, seen.size()));
    const std::size_t unknown[2] = {a[0], 0x10};
    CHECK(!write_physical(unknown, 2, 0, data.data(), kPageBytes + 1));
    CHECK(g6shim_XMemFreePhysicalPages(2, a.data()) == 1);
    CHECK(g6shim_XMemFreePhysicalPages(1, b.data()) == 1);
}

TEST(xmem_page_windows_cover_one_run_in_place) {
    const std::vector<std::size_t> a = allocate_pages(3);
    const std::vector<std::size_t> b = allocate_pages(1);
    {
        PageWindow window;
        CHECK(open_page_window(a.data(), a.size(), kPageBytes + 3, kPageBytes, &window));
        CHECK(window.bytes().size() == kPageBytes);
        std::memset(window.bytes().data(), 0x5a, window.bytes().size());
    }
    const std::vector<unsigned char> second = page_bytes(a[1]);
    const std::vector<unsigned char> third = page_bytes(a[2]);
    CHECK(all_zero(second, 0, 3));
    bool middle = true;
    for (std::size_t i = 3; i < second.size(); ++i) middle = middle && second[i] == 0x5a;
    CHECK(middle);
    bool tail = true;
    for (std::size_t i = 0; i < 3; ++i) tail = tail && third[i] == 0x5a;
    CHECK(tail);
    CHECK(all_zero(third, 3, third.size()));

    const std::size_t mixed[2] = {a[0], b[0]};
    PageWindow window;
    CHECK(!open_page_window(mixed, 2, 0, kPageBytes + 1, &window));
    const std::size_t reversed[2] = {a[1], a[0]};
    CHECK(!open_page_window(reversed, 2, 0, kPageBytes + 1, &window));
    const std::size_t unknown[1] = {0x10};
    CHECK(!open_page_window(unknown, 1, 0, 1, &window));
    CHECK(open_page_window(mixed, 2, 0, kPageBytes, &window));
    CHECK(g6shim_XMemFreePhysicalPages(3, a.data()) == 1);
    CHECK(g6shim_XMemFreePhysicalPages(1, b.data()) == 1);
}

TEST(xmem_commit_limit_report_names_the_request_and_system_commit) {
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof info;
    info.CommitTotal = 512;
    info.CommitLimit = 768;
    info.PageSize = 4096;
    const std::string report = commit_limit_message(4 * 1024 * 1024, 1455, &info);
    CHECK(report.find("4194304 bytes") != std::string::npos);
    CHECK(report.find("2 MiB of 3 MiB in use") != std::string::npos);
    const std::string fallback = commit_limit_message(4 * 1024 * 1024, 1455, nullptr);
    for (const std::string* text : {&report, &fallback}) {
        CHECK(text->find("error 1455") != std::string::npos);
        CHECK(text->find("Windows paging file size") != std::string::npos);
    }
    CHECK(fallback.find("System commit:") == std::string::npos);
}

TEST(xmem_remaps_reach_the_runtime_observer) {
    g_remaps.clear();
    g6shim::set_title_memory_remap_observer(&record_remap);
    const std::vector<std::size_t> pages = allocate_pages(2);
    void* a = g6shim_XMemVirtualAlloc(nullptr, 2 * kPageBytes, MEM_RESERVE | kPages64K, 0, PAGE_READWRITE);
    CHECK(a != nullptr);
    CHECK(g6shim_XMemMapPhysicalPages(a, 2, pages.data()) == a);
    CHECK(g_remaps.count(a) == 1);
    void* second = static_cast<unsigned char*>(a) + kPageBytes;
    CHECK(g6shim_XMemMapPhysicalPages(second, 1, nullptr) == second);
    CHECK(g_remaps.at(a) == 3);
    CHECK(g_remaps.count(second) == 0);
    CHECK(g_remaps.count(static_cast<unsigned char*>(a) + 2 * kPageBytes) == 0);
    g6shim::set_title_memory_remap_observer(nullptr);
    CHECK(g6shim_XMemFreePhysicalPages(2, pages.data()) == 1);
    VirtualFree(a, 0, MEM_RELEASE);
}
