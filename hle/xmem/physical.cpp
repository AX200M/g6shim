// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "physical.hpp"

#include "statistics.hpp"

#include <g6shim/log.hpp>
#include <g6shim/title_memory.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace g6shim::xmem {

namespace {

struct Section {
    explicit Section(HANDLE handle) : handle(handle) {}
    ~Section() {
        if (handle != nullptr) CloseHandle(handle);
    }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    HANDLE handle;
};

struct Slice {
    std::shared_ptr<Section> section;
    std::size_t index;
};

struct Run {
    Slice slice;
    std::size_t pages;
};

struct MappedView {
    std::size_t bytes;
    Slice slice;
};

struct Physical {
    std::size_t next_page = kFirstPageNumber;
    std::unordered_map<std::size_t, Slice> pages;
    std::map<std::size_t, MappedView> views;
};

std::mutex& physical_mutex() {
    static std::mutex mutex;
    return mutex;
}

Physical& physical_state() {
    static Physical physical;
    return physical;
}

bool on_wine() {
    static const bool wine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return wine;
}

void notify_remap(std::size_t address, std::size_t bytes) {
    g6shim::title_memory_remapped(reinterpret_cast<const void*>(address), bytes);
}

std::string os_error_text(std::uint32_t error) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = length != 0 && buffer != nullptr ? std::string(buffer, length) : std::string();
    if (buffer != nullptr) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text.empty()) text = "error " + std::to_string(error);
    return text + " (os error " + std::to_string(error) + ")";
}

bool commit_pages(std::size_t base, std::size_t bytes) {
    if (on_wine()) return true;
    if (VirtualAlloc(reinterpret_cast<void*>(base), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr) return true;

    const DWORD error = GetLastError();
    if (error == ERROR_COMMITMENT_LIMIT) {
        PERFORMANCE_INFORMATION info{};
        info.cb = static_cast<DWORD>(sizeof(info));
        const bool queried = K32GetPerformanceInfo(&info, info.cb) != 0;
        const std::string report = commit_limit_message(bytes, error, queried ? &info : nullptr);
        G6SHIM_ERROR("%s", report.c_str());
    } else {
        G6SHIM_WARN("committing %zu bytes of physical pages failed: error %lu", bytes, error);
    }
    return false;
}

struct UnmapView {
    void operator()(void* address) const {
        if (address != nullptr) UnmapViewOfFile(address);
    }
};

using HostView = std::unique_ptr<void, UnmapView>;

HostView map_view(const Slice& slice, std::size_t pages) {
    const std::uint64_t offset = static_cast<std::uint64_t>(slice.index) * kPageBytes;
    void* at = MapViewOfFile(slice.section->handle, FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32),
                             static_cast<DWORD>(offset), pages * kPageBytes);
    if (at == nullptr) return {};
    HostView view(at);
    if (!commit_pages(reinterpret_cast<std::size_t>(at), pages * kPageBytes)) return {};
    return view;
}

std::optional<std::vector<Run>> runs(const std::size_t* pages, std::size_t count, std::size_t offset,
                                     std::size_t bytes) {
    if ((pages == nullptr && count != 0) || bytes > std::numeric_limits<std::size_t>::max() - offset)
        return std::nullopt;
    const std::size_t end = offset + bytes;
    const std::size_t first = offset / kPageBytes;
    const std::size_t last = end / kPageBytes + (end % kPageBytes == 0 ? 0 : 1);
    if (first > count || last > count) return std::nullopt;

    std::vector<Run> groups;
    std::lock_guard<std::mutex> lock(physical_mutex());
    const Physical& physical = physical_state();
    for (std::size_t i = first; i < last; ++i) {
        const auto it = physical.pages.find(pages[i]);
        if (it == physical.pages.end()) return std::nullopt;
        if (!groups.empty() && groups.back().slice.section == it->second.section &&
            it->second.index == groups.back().slice.index + groups.back().pages) {
            ++groups.back().pages;
        } else {
            groups.push_back({it->second, 1});
        }
    }
    return groups;
}

std::optional<Slice> containing_slice(const Physical& physical, std::size_t address) {
    auto it = physical.views.upper_bound(address);
    if (it == physical.views.begin()) return std::nullopt;
    --it;
    if (address >= it->first + it->second.bytes) return std::nullopt;
    Slice slice = it->second.slice;
    slice.index += (address - it->first) / kPageBytes;
    return slice;
}

bool map(Physical& physical, std::size_t base, std::size_t bytes, const Slice& slice) {
    VirtualFree(reinterpret_cast<void*>(base), bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* view = MapViewOfFile3(slice.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(base),
                                static_cast<std::uint64_t>(slice.index) * kPageBytes, bytes,
                                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (reinterpret_cast<std::size_t>(view) != base) {
        G6SHIM_WARN("mapping %zu bytes of physical pages at 0x%llX failed: error %lu (not a placeholder?)", bytes,
                    static_cast<unsigned long long>(base), GetLastError());
        return false;
    }
    if (!commit_pages(base, bytes)) {
        UnmapViewOfFile2(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
        return false;
    }
    notify_remap(base, bytes);
    physical.views[base] = MappedView{bytes, slice};
    return true;
}

void unmap(Physical& physical, std::size_t base, std::size_t bytes) {
    if (bytes == 0) return;
    const std::size_t end = base + bytes;
    std::vector<std::size_t> hit;
    auto containing = physical.views.upper_bound(base);
    if (containing != physical.views.begin()) {
        --containing;
        if (base < containing->first + containing->second.bytes) hit.push_back(containing->first);
    }
    for (auto it = physical.views.upper_bound(base); it != physical.views.end() && it->first < end; ++it)
        hit.push_back(it->first);

    for (const std::size_t at : hit) {
        const auto it = physical.views.find(at);
        if (it == physical.views.end()) continue;
        const MappedView mapped = it->second;
        physical.views.erase(it);
        notify_remap(at, mapped.bytes);
        UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(at), MEM_PRESERVE_PLACEHOLDER);
        if (at < base) map(physical, at, base - at, mapped.slice);
        if (at + mapped.bytes > end) {
            Slice tail = mapped.slice;
            tail.index += (end - at) / kPageBytes;
            map(physical, end, at + mapped.bytes - end, tail);
        }
    }
}

}  // namespace

void PageWindow::close() noexcept {
    if (mapping_ != nullptr) UnmapViewOfFile(mapping_);
    mapping_ = nullptr;
    within_ = 0;
    length_ = 0;
}

PageWindow::~PageWindow() { close(); }

PageWindow::PageWindow(PageWindow&& other) noexcept
    : mapping_(other.mapping_), within_(other.within_), length_(other.length_) {
    other.mapping_ = nullptr;
    other.within_ = 0;
    other.length_ = 0;
}

PageWindow& PageWindow::operator=(PageWindow&& other) noexcept {
    if (this != &other) {
        close();
        mapping_ = other.mapping_;
        within_ = other.within_;
        length_ = other.length_;
        other.mapping_ = nullptr;
        other.within_ = 0;
        other.length_ = 0;
    }
    return *this;
}

std::span<unsigned char> PageWindow::bytes() noexcept {
    if (mapping_ == nullptr) return {};
    return {static_cast<unsigned char*>(mapping_) + within_, length_};
}

bool find_physical_page(std::size_t number, PhysicalPage* page) {
    if (page == nullptr) return false;
    std::lock_guard<std::mutex> lock(physical_mutex());
    const auto it = physical_state().pages.find(number);
    if (it == physical_state().pages.end()) return false;
    page->section = it->second.section->handle;
    page->index = it->second.index;
    return true;
}

bool find_physical_mapping(std::size_t address, PhysicalMapping* mapping) {
    if (mapping == nullptr) return false;
    std::lock_guard<std::mutex> lock(physical_mutex());
    const Physical& physical = physical_state();
    const auto slice = containing_slice(physical, address);
    if (!slice) return false;
    auto it = physical.views.upper_bound(address);
    --it;
    mapping->address = it->first;
    mapping->bytes = it->second.bytes;
    mapping->page = PhysicalPage{slice->section->handle, slice->index};
    return true;
}

bool page_runs(const std::size_t* pages, std::size_t count, std::size_t offset, std::size_t bytes,
               std::vector<PageRun>* out) {
    if (out == nullptr) return false;
    out->clear();
    const auto groups = runs(pages, count, offset, bytes);
    if (!groups) return false;
    out->reserve(groups->size());
    for (const Run& run : *groups) out->push_back(PageRun{run.slice.section->handle, run.slice.index, run.pages});
    return true;
}

bool open_page_window(const std::size_t* pages, std::size_t count, std::size_t offset, std::size_t bytes,
                      PageWindow* window) {
    if (window == nullptr) return false;
    const auto groups = runs(pages, count, offset, bytes);
    if (!groups || groups->size() != 1) return false;
    const Run& run = groups->front();
    HostView view = map_view(run.slice, run.pages);
    if (!view) return false;
    window->close();
    window->mapping_ = view.release();
    window->within_ = offset % kPageBytes;
    window->length_ = bytes;
    return true;
}

bool write_physical(const std::size_t* pages, std::size_t count, std::size_t offset, const void* data,
                    std::size_t bytes) {
    if (data == nullptr && bytes != 0) return false;
    const auto groups = runs(pages, count, offset, bytes);
    if (!groups) return false;

    std::size_t within = offset % kPageBytes;
    std::size_t done = 0;
    const auto* source = static_cast<const unsigned char*>(data);
    for (const Run& run : *groups) {
        HostView view = map_view(run.slice, run.pages);
        if (!view) return false;
        const std::size_t room = run.pages * kPageBytes - within;
        const std::size_t copied = std::min(room, bytes - done);
        if (copied != 0)
            std::memcpy(static_cast<unsigned char*>(view.get()) + within, source + done, copied);
        within = 0;
        done += copied;
    }
    return true;
}

bool map_alias(std::uintptr_t to, std::uintptr_t from, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(physical_mutex());
    Physical& physical = physical_state();
    unmap(physical, to, bytes);
    for (std::size_t k = 0; k < bytes; k += kPageBytes) {
        if (from == 0) break;
        const auto slice = containing_slice(physical, from + k);
        if (!slice) return false;
        if (!map(physical, to + k, kPageBytes, *slice)) return false;
    }
    return true;
}

int allocate_physical_pages(std::uint64_t flags, std::size_t* count, std::size_t* pages) {
    if (count == nullptr || pages == nullptr) return 0;
    const std::size_t n = *count;
    if (n == 0 || n > std::numeric_limits<std::size_t>::max() / kPageBytes) return 0;
    const std::size_t bytes = n * kPageBytes;
    const HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                             PAGE_READWRITE | (on_wine() ? SEC_COMMIT : SEC_RESERVE),
                                             static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (handle == nullptr) {
        G6SHIM_WARN("XMemAllocatePhysicalPages: %zu pages failed: error %lu", n, GetLastError());
        *count = 0;
        return 0;
    }
    G6SHIM_TRACE("XMemAllocatePhysicalPages %zu pages, flags 0x%llX", n,
                 static_cast<unsigned long long>(flags));

    const auto section = std::make_shared<Section>(handle);
    std::lock_guard<std::mutex> lock(physical_mutex());
    Physical& physical = physical_state();
    const std::size_t first = physical.next_page;
    physical.next_page += n * kPageFrames;
    for (std::size_t index = 0; index < n; ++index) {
        const std::size_t number = first + index * kPageFrames;
        physical.pages.emplace(number, Slice{section, index});
        pages[index] = number;
    }
    return 1;
}

int free_physical_pages(std::size_t n, const std::size_t* pages) {
    if (pages == nullptr) return 0;
    std::lock_guard<std::mutex> lock(physical_mutex());
    Physical& physical = physical_state();
    for (std::size_t i = 0; i < n; ++i) physical.pages.erase(pages[i]);
    return 1;
}

void* map_physical_pages(void* address, std::size_t n, const std::size_t* pages) {
    if (n > std::numeric_limits<std::size_t>::max() / kPageBytes) return nullptr;
    const std::size_t base = reinterpret_cast<std::size_t>(address);
    std::lock_guard<std::mutex> lock(physical_mutex());
    Physical& physical = physical_state();
    unmap(physical, base, n * kPageBytes);
    if (pages == nullptr) return address;

    std::size_t i = 0;
    while (i < n) {
        const auto it = physical.pages.find(pages[i]);
        if (it == physical.pages.end()) {
            G6SHIM_WARN("XMemMapPhysicalPages: unknown page 0x%zX", pages[i]);
            return nullptr;
        }
        const Slice first = it->second;
        std::size_t run = 1;
        while (i + run < n) {
            const auto next = physical.pages.find(pages[i + run]);
            if (next == physical.pages.end() || next->second.section != first.section ||
                next->second.index != first.index + run)
                break;
            ++run;
        }
        if (!map(physical, base + i * kPageBytes, run * kPageBytes, first)) return nullptr;
        i += run;
    }
    return address;
}

int map_physical_pages_scatter(void* const* addresses, std::size_t n, const std::size_t* pages) {
    if (addresses == nullptr) return 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t* page = pages == nullptr || pages[i] == 0 ? nullptr : pages + i;
        if (map_physical_pages(addresses[i], 1, page) == nullptr) return 0;
    }
    return 1;
}

std::string commit_limit_message(std::size_t bytes, std::uint32_t error, const PERFORMANCE_INFORMATION* info) {
    std::string message = "g6shim: Windows commit limit reached while committing " + std::to_string(bytes) +
                          " bytes of physical pages: " + os_error_text(error) + ".\n";
    if (info != nullptr) {
        const std::size_t committed = info->CommitTotal * info->PageSize / kMebibyte;
        const std::size_t limit = info->CommitLimit * info->PageSize / kMebibyte;
        message += "System commit: " + std::to_string(committed) + " MiB of " + std::to_string(limit) +
                   " MiB in use.\n";
    }
    message += "Close other applications or increase the Windows paging file size, then retry.\n";
    return message;
}

}  // namespace g6shim::xmem

using namespace g6shim::xmem;

extern "C" BOOL WINAPI g6shim_XMemAllocatePhysicalPages(ULONGLONG flags, SIZE_T* count, SIZE_T* pages) {
    return allocate_physical_pages(flags, count, pages);
}

extern "C" BOOL WINAPI g6shim_XMemFreePhysicalPages(SIZE_T n, const SIZE_T* pages) {
    return free_physical_pages(n, pages);
}

extern "C" void* WINAPI g6shim_XMemMapPhysicalPages(void* address, SIZE_T n, const SIZE_T* pages) {
    return map_physical_pages(address, n, pages);
}

extern "C" BOOL WINAPI g6shim_XMemMapPhysicalPagesScatter(void* const* addresses, SIZE_T n,
                                                          const SIZE_T* pages) {
    return map_physical_pages_scatter(addresses, n, pages);
}

extern "C" BOOL WINAPI g6shim_xmem_map_alias(std::uintptr_t to, std::uintptr_t from, std::size_t bytes) {
    return map_alias(to, from, bytes) ? TRUE : FALSE;
}
