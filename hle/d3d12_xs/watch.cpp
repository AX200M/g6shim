// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "watch.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace g6shim::d3d12_xs::watch {

namespace {

constexpr std::uint32_t kResetWrites = 1;

inline std::size_t page_of(std::size_t address) { return address / kPageBytes; }
inline std::size_t page_end(std::size_t address) {
    return address / kPageBytes + (address % kPageBytes != 0 ? 1 : 0);
}

inline std::size_t clamped_end(std::size_t at, std::size_t bytes) {
    return bytes > std::numeric_limits<std::size_t>::max() - at ? std::numeric_limits<std::size_t>::max()
                                                                : at + bytes;
}

struct Journal {
    std::uint64_t asked = 0;
    std::map<std::size_t, std::uint64_t> written;
    std::vector<std::size_t> found;
};

std::mutex g_journal_mutex;
Journal g_journal;

std::mutex g_guards_mutex;
std::set<std::size_t> g_protected;
std::set<std::size_t> g_clean;
std::atomic<std::uint64_t> g_remaps{0};

void install();

bool protect(std::size_t from, std::size_t to) {
    DWORD old = 0;
    return ::VirtualProtect(reinterpret_cast<void*>(from * kPageBytes), (to - from) * kPageBytes, PAGE_READONLY,
                            &old) != 0;
}

void note_protected(std::size_t from, std::size_t to) {
    for (std::size_t block = from; block < to; ++block) {
        g_protected.insert(block);
        g_clean.insert(block);
    }
}

void guard_blocks(std::size_t from, std::size_t to) {
    if (from >= to) return;
    if (protect(from, to)) {
        note_protected(from, to);
        return;
    }
    for (std::size_t block = from; block < to; ++block)
        if (protect(block, block + 1)) note_protected(block, block + 1);
}

std::optional<std::vector<std::size_t>> guard(std::size_t start, std::size_t end) {
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<const void*>(start), &info, sizeof info) == 0 || info.Type != MEM_MAPPED)
        return std::nullopt;
    install();
    std::lock_guard<std::mutex> lock(g_guards_mutex);
    std::vector<std::size_t> written;
    std::optional<std::size_t> run;
    const std::size_t first = page_of(start);
    const std::size_t last = page_end(end);
    for (std::size_t block = first; block <= last; ++block) {
        const bool open = block < last && g_clean.find(block) == g_clean.end();
        if (open) {
            written.push_back(block);
            if (g_protected.find(block) != g_protected.end()) {
                g_clean.insert(block);
            } else {
                if (!run) run = block;
                continue;
            }
        }
        if (run) {
            guard_blocks(*run, block);
            run.reset();
        }
    }
    return written;
}

bool open(std::size_t start, std::size_t end) {
    std::vector<std::size_t> blocks;
    for (auto it = g_protected.lower_bound(page_of(start)); it != g_protected.end() && *it < page_end(end); ++it)
        blocks.push_back(*it);
    for (const std::size_t block : blocks) {
        g_protected.erase(block);
        g_clean.erase(block);
        DWORD old = 0;
        ::VirtualProtect(reinterpret_cast<void*>(block * kPageBytes), kPageBytes, PAGE_READWRITE, &old);
    }
    return !blocks.empty();
}

bool writable(std::size_t at) {
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info) == 0) return false;
    return info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE;
}

LONG CALLBACK on_write(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    if (record == nullptr || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto at = static_cast<std::size_t>(record->ExceptionInformation[1]);
    {
        std::lock_guard<std::mutex> lock(g_guards_mutex);
        if (open(at, at + 1)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (writable(at)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

void install() {
    static std::once_flag once;
    std::call_once(once, [] { ::AddVectoredExceptionHandler(1, &on_write); });
}

}  // namespace

std::optional<Pages> writes(std::size_t start, std::size_t end, bool guarded) {
    const std::size_t first = page_of(start);
    const std::size_t last = page_end(end);
    if (last < first) return std::nullopt;
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    std::size_t count = (last - first) * kPageBytes / kKernelPageBytes;
    g_journal.found.resize(std::max(count, g_journal.found.size()), 0);
    DWORD granularity = 0;
    ULONG_PTR entries = count;
    const bool failed =
        ::GetWriteWatch(kResetWrites, reinterpret_cast<void*>(first * kPageBytes), (last - first) * kPageBytes,
                        reinterpret_cast<void**>(g_journal.found.data()), &entries, &granularity) != 0;
    std::vector<std::size_t> guarded_pages;
    if (failed) {
        entries = 0;
        if (!guarded) return std::nullopt;
        const auto found = guard(first * kPageBytes, last * kPageBytes);
        if (!found) return std::nullopt;
        guarded_pages = *found;
    }
    ++g_journal.asked;
    const std::uint64_t now = g_journal.asked;
    for (std::size_t i = 0; i < entries; ++i) g_journal.written[g_journal.found[i] / kPageBytes] = now;
    for (const std::size_t page : guarded_pages) g_journal.written[page] = now;
    Pages pages;
    pages.asked = now;
    for (auto it = g_journal.written.lower_bound(first); it != g_journal.written.end() && it->first < last; ++it)
        pages.written.emplace_back(it->first, it->second);
    return pages;
}

void writing(std::size_t at, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_guards_mutex);
    open(at, clamped_end(at, bytes));
}

std::uint64_t remaps() { return g_remaps.load(std::memory_order_acquire); }

void title_memory_remapped(std::size_t at, std::size_t bytes) {
    g_remaps.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(g_guards_mutex);
    const std::size_t end = clamped_end(at, bytes);
    std::vector<std::size_t> blocks;
    for (auto it = g_protected.lower_bound(page_of(at)); it != g_protected.end() && *it < page_end(end); ++it)
        blocks.push_back(*it);
    for (const std::size_t block : blocks) {
        g_protected.erase(block);
        g_clean.erase(block);
    }
}

}  // namespace g6shim::d3d12_xs::watch
