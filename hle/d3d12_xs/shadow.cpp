// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shadow.hpp"

#include "card.hpp"
#include "watch.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace g6shim::d3d12_xs::shadow {

namespace {

constexpr std::uint8_t kWatched = 1;
constexpr std::uint8_t kNotWatched = 2;
constexpr std::size_t kPoolSize = 8;
constexpr std::uint64_t kWindowBytes = 4 << 20;
constexpr std::uint64_t kCopyAlignment = 1 << 16;
constexpr std::size_t kBackPieceBytes = 64 << 10;

std::mutex g_pool_mutex;
std::vector<std::vector<std::uint8_t>> g_pool;

std::vector<std::uint8_t> pooled(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    std::optional<std::size_t> fit;
    for (std::size_t i = 0; i < g_pool.size(); ++i) {
        if (g_pool[i].capacity() < bytes) continue;
        if (!fit || g_pool[i].capacity() < g_pool[*fit].capacity()) fit = i;
    }
    std::vector<std::uint8_t> out;
    if (fit) {
        out = std::move(g_pool[*fit]);
        g_pool.erase(g_pool.begin() + static_cast<std::ptrdiff_t>(*fit));
    }
    out.clear();
    if (out.capacity() < bytes) out.reserve(bytes);
    return out;
}

void give_back(std::vector<std::uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    g_pool.push_back(std::move(bytes));
    if (g_pool.size() > kPoolSize) {
        std::sort(g_pool.begin(), g_pool.end(),
                  [](const auto& a, const auto& b) { return a.capacity() > b.capacity(); });
        g_pool.resize(kPoolSize);
    }
}

struct Entry {
    std::uint64_t end = 0;
    std::shared_ptr<Shadow> shadow;
};

using BufferKey = std::pair<std::uint64_t, std::uintptr_t>;

std::mutex g_buffers_mutex;
std::map<BufferKey, Entry> g_buffers;

std::mutex g_loose_mutex;
std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<Shadow>> g_loose;
std::unordered_set<ID3D12Resource*> g_loose_buffers;
std::atomic<std::uint64_t> g_submission{0};
std::atomic<std::uint64_t> g_generation{0};
std::atomic<std::uint64_t> g_buffers_generation{1};
std::atomic<std::uint64_t> g_evicted{0};
std::atomic<std::uint64_t> g_loose_budget{kLooseBudgetBytes};
std::atomic<std::uint64_t> g_back_nanoseconds{0};
std::atomic<std::uint64_t> g_back_ranges{0};
std::atomic<std::uint64_t> g_back_compared{0};
std::atomic<std::uint64_t> g_back_read{0};
std::atomic<std::uint64_t> g_back_written{0};

std::optional<std::shared_ptr<Shadow>> loose(std::uint64_t address, std::uint64_t end) {
    const std::uint64_t start = address & ~(kWindowBytes - 1);
    const std::uint64_t rounded = (end + kWindowBytes - 1) / kWindowBytes * kWindowBytes;
    const std::uint64_t bytes = rounded - start;
    {
        std::lock_guard<std::mutex> lock(g_loose_mutex);
        const auto found = g_loose.find({start, bytes});
        if (found != g_loose.end()) {
            found->second->used(g_submission.load(std::memory_order_relaxed));
            return found->second;
        }
    }
    host::Host* gpu_host = host::host();
    if (gpu_host == nullptr) return std::nullopt;
    auto buffer = gpu_host->shared_buffer(bytes);
    if (!buffer) return std::nullopt;
    host::name(buffer->first.Get(), "title memory");
    const std::uint64_t gpu = buffer->first->GetGPUVirtualAddress();
    G6SHIM_TRACE("d3d12_xs: loose title memory %llx+%llx copied to the host",
                 static_cast<unsigned long long>(start), static_cast<unsigned long long>(bytes));
    auto shadow = std::make_shared<Shadow>(std::move(buffer->first), buffer->second, gpu, start, bytes);
    std::lock_guard<std::mutex> lock(g_loose_mutex);
    const auto [at, inserted] = g_loose.try_emplace({start, bytes}, shadow);
    at->second->used(g_submission.load(std::memory_order_relaxed));
    if (inserted) g_loose_buffers.insert(shadow->buffer());
    return at->second;
}

}  // namespace

Shadow::Shadow(Microsoft::WRL::ComPtr<ID3D12Resource> buffer, std::uint8_t* data, std::uint64_t gpu,
               std::uint64_t start, std::uint64_t bytes)
    : buffer_(std::move(buffer)), data_(data), gpu_(gpu), start_(start), bytes_(bytes) {
    const std::size_t first = static_cast<std::size_t>(start_ / watch::kPageBytes);
    const std::size_t last =
        static_cast<std::size_t>((start_ + bytes_ + watch::kPageBytes - 1) / watch::kPageBytes);
    copied_.assign(last > first ? last - first : 0, 0);
}

Shadow::~Shadow() = default;

void Shadow::bind_card(std::shared_ptr<g6shim::d3d12_xs::card::Side> side) { card_ = std::move(side); }

void Shadow::arm_card(std::size_t address, std::size_t bytes) {
    if (!card_) return;
    const auto [from, to] = inside(address, bytes);
    if (from < to) card_->note_writes(from, to - from);
}

std::size_t Shadow::fetched_pieces() const { return card_ ? card_->fetched_count() : 0; }

std::uint64_t card_minimum_bytes() {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return 16ull << 20;
    return card::minimum_bytes(gpu->device());
}
std::uint64_t Shadow::left(std::uint64_t address) const {
    const std::uint64_t end = start_ + bytes_;
    return end > address ? end - address : 0;
}

void Shadow::refresh(host::HostList& list, std::uint64_t address, std::uint64_t bytes) {
    const std::uint64_t len = std::min(bytes, left(address));
    list.defer_refresh(shared_from_this(), static_cast<std::size_t>(address), static_cast<std::size_t>(len));
}

void Shadow::gpu_writes(host::HostList& list, std::uint64_t address, std::uint64_t bytes) {
    const std::uint64_t len = std::min(bytes, left(address));
    list.defer_gpu_write(shared_from_this(), static_cast<std::size_t>(address), static_cast<std::size_t>(len));
}

std::pair<std::size_t, std::size_t> Shadow::inside(std::size_t address, std::size_t bytes) const {
    const std::size_t start = static_cast<std::size_t>(start_);
    const std::size_t end = static_cast<std::size_t>(start_ + bytes_);
    const std::size_t from = std::clamp(address, start, end);
    const std::size_t room = end > address ? end - address : 0;
    const std::size_t requested = bytes > room ? end : address + bytes;
    const std::size_t to = std::clamp(requested, from, end);
    return {from - start, to - start};
}

std::size_t Shadow::writing(std::size_t address, std::size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.emplace_back(address, address + bytes);
    }
    const auto [from, to] = inside(address, bytes);
    std::lock_guard<std::mutex> lock(before_mutex_);
    std::vector<std::pair<std::size_t, std::size_t>> gaps;
    std::size_t at = from;
    auto next = before_.upper_bound(from);
    if (next != before_.begin()) {
        const auto previous = std::prev(next);
        at = std::max(at, previous->first + previous->second.size());
    }
    while (at < to) {
        const std::size_t stop = next == before_.end() ? to : std::min(to, next->first);
        if (at < stop) gaps.emplace_back(at, stop);
        if (next == before_.end()) break;
        at = std::max(at, next->first + next->second.size());
        ++next;
    }
    std::size_t captured = 0;
    for (const auto& [gap_from, gap_to] : gaps) {
        std::vector<std::uint8_t> held = pooled(gap_to - gap_from);
        held.resize(gap_to - gap_from);
        const auto* title = reinterpret_cast<const std::uint8_t*>(static_cast<std::size_t>(start_) + gap_from);
        if (!card_ || !host::read_into(title, std::span<std::uint8_t>(held.data(), held.size())))
            std::memcpy(held.data(), data_ + gap_from, held.size());
        captured += held.size();
        before_.emplace(gap_from, std::move(held));
    }
    return captured;
}

std::optional<std::vector<std::uint8_t>> Shadow::before_of(std::size_t from, std::size_t to) const {
    if (from >= to) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(to - from);
    std::size_t at = from;
    auto next = before_.upper_bound(from);
    if (next != before_.begin()) --next;
    for (; next != before_.end() && at < to; ++next) {
        const std::size_t part_from = next->first;
        const std::size_t part_to = part_from + next->second.size();
        if (part_to <= at) continue;
        if (part_from > at) return std::nullopt;
        const std::size_t take = std::min(to, part_to) - at;
        out.insert(out.end(), next->second.begin() + static_cast<std::ptrdiff_t>(at - part_from),
                   next->second.begin() + static_cast<std::ptrdiff_t>(at - part_from + take));
        at += take;
    }
    if (at < to) return std::nullopt;
    return out;
}

void Shadow::drop_before() {
    for (auto& held : before_) give_back(std::move(held.second));
    before_.clear();
}

std::size_t Shadow::merge_card(std::size_t address, std::size_t bytes) {
    if (!card_) return 0;
    const auto [from, to] = inside(address, bytes);
    const auto before = before_of(from, to);
    if (!before) return 0;
    const std::size_t start = static_cast<std::size_t>(start_);
    std::size_t written = 0;
    constexpr std::size_t kPiece = static_cast<std::size_t>(card::kPieceBytes);
    const std::size_t last = (to - 1) / kPiece;
    for (std::size_t piece = from / kPiece; piece <= last; ++piece) {
        const std::uint8_t* gpu = card_->fetched(static_cast<std::uint32_t>(piece));
        if (gpu == nullptr) continue;
        const std::size_t piece_from = piece * kPiece;
        const std::size_t piece_to = std::min(piece_from + kPiece, static_cast<std::size_t>(bytes_));
        const std::size_t lo = std::max(from, piece_from);
        const std::size_t hi = std::min(to, piece_to);
        if (lo >= hi) continue;
        const std::size_t n = hi - lo;
        std::vector<std::uint8_t> title(n);
        if (!host::read_into(reinterpret_cast<const std::uint8_t*>(start + lo),
                             std::span<std::uint8_t>(title.data(), n)))
            continue;
        const std::vector<std::uint8_t> was = title;
        bool changed = false;
        bool kept = false;
        std::size_t i = 0;
        while (i + 4 <= n && ((lo + i) & 3u) == 0) {
            std::uint32_t gpu_dw = 0;
            std::uint32_t old_dw = 0;
            std::uint32_t title_dw = 0;
            std::memcpy(&gpu_dw, gpu + (lo - piece_from) + i, 4);
            std::memcpy(&old_dw, before->data() + (lo - from) + i, 4);
            std::memcpy(&title_dw, title.data() + i, 4);
            if (gpu_dw != old_dw) {
                if (title_dw == old_dw) {
                    std::memcpy(title.data() + i, &gpu_dw, 4);
                    changed = true;
                } else {
                    kept = true;
                }
            }
            std::memcpy(data_ + lo + i, title.data() + i, 4);
            i += 4;
        }
        for (; i < n; ++i) {
            const std::uint8_t gpu_b = gpu[(lo - piece_from) + i];
            const std::uint8_t old_b = (*before)[(lo - from) + i];
            if (gpu_b != old_b) {
                if (title[i] == old_b) {
                    title[i] = gpu_b;
                    changed = true;
                } else {
                    kept = true;
                }
            }
            data_[lo + i] = title[i];
        }
        if (changed) {
            watch::writing(start + lo, n);
            written += put_changed(reinterpret_cast<std::uint8_t*>(start + lo), was.data(), title.data(), n);
        }
        if (kept) card_->note_kept(static_cast<std::uint32_t>(piece), lo, n);
    }
    return written;
}

std::optional<std::pair<std::size_t, std::size_t>> changed_span(const std::uint8_t* a, const std::uint8_t* b,
                                                               std::size_t bytes) {
    constexpr std::size_t kBlock = 64;
    std::size_t first = 0;
    while (first + kBlock <= bytes && std::memcmp(a + first, b + first, kBlock) == 0) first += kBlock;
    while (first < bytes && a[first] == b[first]) ++first;
    if (first == bytes) return std::nullopt;
    std::size_t end = bytes;
    while (end - first >= kBlock && std::memcmp(a + end - kBlock, b + end - kBlock, kBlock) == 0) end -= kBlock;
    while (end > first && a[end - 1] == b[end - 1]) --end;
    return std::make_pair(first, end);
}

std::size_t put_changed(std::uint8_t* destination, const std::uint8_t* was, const std::uint8_t* now,
                        std::size_t bytes) {
    constexpr std::size_t kDword = 4;
    constexpr std::size_t kBlock = 32;
    const auto base = reinterpret_cast<std::uintptr_t>(destination);
    std::size_t written = 0;
    std::size_t run = 0;
    bool open = false;
    const auto flush = [&](std::size_t end) {
        if (!open) return;
        host::write_mapped(destination + run, now + run, end - run);
        written += end - run;
        open = false;
    };
    std::size_t at = 0;
    while (at < bytes) {
        const bool aligned = (base + at) % kDword == 0;
        if (aligned && at + kBlock <= bytes && std::memcmp(was + at, now + at, kBlock) == 0) {
            flush(at);
            at += kBlock;
            continue;
        }
        const std::size_t unit = std::min(kDword - (base + at) % kDword, bytes - at);
        if (std::memcmp(was + at, now + at, unit) != 0) {
            if (!open) {
                run = at;
                open = true;
            }
        } else {
            flush(at);
        }
        at += unit;
    }
    flush(bytes);
    return written;
}

void copy_back_all(std::span<const WriteRange> writes, CopyBackStats* stats) {
    const auto began = std::chrono::steady_clock::now();
    std::map<Shadow*, std::vector<std::pair<std::size_t, std::size_t>>> by_shadow;
    for (const auto& [copy, address, bytes] : writes)
        if (copy && bytes > 0) by_shadow[copy.get()].emplace_back(address, address + bytes);
    for (auto& [copy, ranges] : by_shadow) {
        std::sort(ranges.begin(), ranges.end());
        ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
        std::size_t union_from = ranges.front().first;
        std::size_t union_to = ranges.front().second;
        const auto merge = [&](std::size_t from, std::size_t to) {
            const BackCounts counts = copy->merge_back(from, to - from);
            if (stats != nullptr) {
                ++stats->ranges;
                stats->compared += counts.compared;
                stats->read += counts.read;
                stats->written += counts.written;
            }
        };
        for (const auto& [from, to] : ranges) {
            if (from <= union_to) {
                union_to = std::max(union_to, to);
                continue;
            }
            merge(union_from, union_to);
            union_from = from;
            union_to = to;
        }
        merge(union_from, union_to);
        copy->finish_writes(ranges);
    }
    if (stats != nullptr)
        stats->nanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
}

void note_copy_back_stats(const CopyBackStats& stats) {
    g_back_nanoseconds.fetch_add(stats.nanoseconds, std::memory_order_relaxed);
    g_back_ranges.fetch_add(stats.ranges, std::memory_order_relaxed);
    g_back_compared.fetch_add(stats.compared, std::memory_order_relaxed);
    g_back_read.fetch_add(stats.read, std::memory_order_relaxed);
    g_back_written.fetch_add(stats.written, std::memory_order_relaxed);
}

CopyBackStats take_copy_back_stats() {
    CopyBackStats stats;
    stats.nanoseconds = g_back_nanoseconds.exchange(0, std::memory_order_relaxed);
    stats.ranges = g_back_ranges.exchange(0, std::memory_order_relaxed);
    stats.compared = g_back_compared.exchange(0, std::memory_order_relaxed);
    stats.read = g_back_read.exchange(0, std::memory_order_relaxed);
    stats.written = g_back_written.exchange(0, std::memory_order_relaxed);
    return stats;
}

bool merge_written(std::uint8_t* title, std::uint8_t* host, const std::uint8_t* old, std::size_t bytes) {
    constexpr std::size_t kBlock = 32;
    bool changed = false;
    const auto by_byte = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            if (title[i] != old[i]) {
                host[i] = title[i];
            } else if (host[i] != old[i]) {
                title[i] = host[i];
                changed = true;
            }
        }
    };
    std::size_t at = 0;
    for (; at + kBlock <= bytes; at += kBlock) {
        const bool title_kept = std::memcmp(title + at, old + at, kBlock) == 0;
        const bool host_kept = std::memcmp(host + at, old + at, kBlock) == 0;
        if (title_kept && host_kept) continue;
        if (title_kept) {
            std::memcpy(title + at, host + at, kBlock);
            changed = true;
        } else if (host_kept) {
            std::memcpy(host + at, title + at, kBlock);
        } else {
            by_byte(at, at + kBlock);
        }
    }
    by_byte(at, bytes);
    return changed;
}

BackCounts Shadow::merge_back(std::size_t address, std::size_t bytes) {
    BackCounts counts;
    std::lock_guard<std::mutex> snapshot_lock(before_mutex_);
    const auto [from, to] = inside(address, bytes);
    counts.compared = to > from ? to - from : 0;
    if (card_) {
        counts.written = merge_card(address, bytes);
        return counts;
    }
    const std::size_t start = static_cast<std::size_t>(start_);
    std::vector<std::uint8_t> title;
    std::vector<std::uint8_t> was;
    auto part = before_.upper_bound(from);
    if (part != before_.begin()) --part;
    for (; part != before_.end() && part->first < to; ++part) {
        const std::size_t part_from = std::max(from, part->first);
        const std::size_t part_to = std::min(to, part->first + part->second.size());
        std::size_t at = part_from;
        while (at < part_to) {
            const std::size_t n = std::min(kBackPieceBytes - (start + at) % kBackPieceBytes, part_to - at);
            const std::uint8_t* old = part->second.data() + (at - part->first);
            if (const auto span = changed_span(data_ + at, old, n)) {
                const std::size_t lo = at + span->first;
                const std::size_t len = span->second - span->first;
                auto* target = reinterpret_cast<std::uint8_t*>(start + lo);
                title.resize(len);
                if (host::read_into(target, std::span<std::uint8_t>(title.data(), len))) {
                    counts.read += len;
                    was.assign(title.begin(), title.end());
                    if (merge_written(title.data(), data_ + lo, old + span->first, len)) {
                        watch::writing(start + lo, len);
                        counts.written += put_changed(target, was.data(), title.data(), len);
                    }
                }
            }
            at += n;
        }
    }
    return counts;
}

void Shadow::finish_writes(const std::vector<std::pair<std::size_t, std::size_t>>& sorted_ranges) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const std::pair<std::size_t, std::size_t>& range) {
                                      return std::binary_search(sorted_ranges.begin(), sorted_ranges.end(), range);
                                  }),
                   pending_.end());
    if (pending_.empty()) {
        std::lock_guard<std::mutex> snapshot_lock(before_mutex_);
        drop_before();
    }
}

std::size_t Shadow::copy_back(std::size_t address, std::size_t bytes) {
    const std::size_t written = merge_back(address, bytes).written;
    finish_writes({std::make_pair(address, address + bytes)});
    return written;
}

void Shadow::discard_write(std::size_t address, std::size_t bytes) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    const auto range = std::make_pair(address, address + bytes);
    pending_.erase(std::remove(pending_.begin(), pending_.end(), range), pending_.end());
    if (pending_.empty()) {
        std::lock_guard<std::mutex> snapshot_lock(before_mutex_);
        drop_before();
    }
}

std::size_t Shadow::update(std::size_t address, std::size_t bytes) {
    host::wait_for_writes(reinterpret_cast<const void*>(address), bytes);
    const std::size_t start = static_cast<std::size_t>(start_);
    const std::size_t end = static_cast<std::size_t>(start_ + bytes_);
    if (address >= end || bytes == 0) return 0;
    const std::size_t from = std::max(address, start);
    const std::size_t to = std::min(bytes > end - address ? end : address + bytes, end);
    if (from >= to) return 0;
    std::vector<std::pair<std::size_t, std::size_t>> busy;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (const auto& range : pending_)
            if (range.first < to && range.second > from) busy.push_back(range);
    }
    if (!busy.empty()) {
        std::sort(busy.begin(), busy.end());
        std::size_t at = from;
        std::size_t copied = 0;
        for (const auto& [a, e] : busy) {
            if (a > at) copied += update_free(at, a);
            at = std::max(at, e);
        }
        if (at < to) copied += update_free(at, to);
        return copied;
    }
    return update_free(from, to);
}

std::size_t Shadow::update_free(std::size_t from, std::size_t to) {
    if (watched_.load(std::memory_order_relaxed) != kNotWatched) {
        const auto pages = watch::writes(from, to, true);
        if (pages) {
            watched_.store(kWatched, std::memory_order_relaxed);
            return copy_stale(from, to, pages->asked, pages->written);
        }
        watched_.store(kNotWatched, std::memory_order_relaxed);
    }
    copy_range(from, to);
    return to - from;
}

std::size_t Shadow::copy_stale(std::size_t from, std::size_t to, std::uint64_t now,
                               const std::vector<std::pair<std::size_t, std::uint64_t>>& written) {
    const std::size_t base = static_cast<std::size_t>(start_ / watch::kPageBytes);
    std::lock_guard<std::mutex> lock(copied_mutex_);
    std::size_t index = 0;
    std::size_t bytes = 0;
    std::optional<std::size_t> run;
    const std::size_t page_start = from / watch::kPageBytes;
    const std::size_t last = (to - 1) / watch::kPageBytes;
    for (std::size_t page = page_start; page <= last; ++page) {
        std::uint64_t write = 0;
        if (index < written.size() && written[index].first == page) {
            write = written[index].second;
            ++index;
        }
        std::uint64_t& at = copied_[page - base];
        if (at == 0 || write > at) {
            if (std::max(page * watch::kPageBytes, static_cast<std::size_t>(start_)) >= from &&
                std::min((page + 1) * watch::kPageBytes, static_cast<std::size_t>(start_ + bytes_)) <= to)
                at = now;
            if (!run) run = page;
        } else if (run) {
            bytes += copy_range(std::max(*run * watch::kPageBytes, from), std::min(page * watch::kPageBytes, to));
            run.reset();
        }
    }
    if (run) bytes += copy_range(std::max(*run * watch::kPageBytes, from), std::min((last + 1) * watch::kPageBytes, to));
    return bytes;
}

std::size_t Shadow::copy_range(std::size_t from, std::size_t to) {
    const std::size_t start = static_cast<std::size_t>(start_);
    const std::size_t end = static_cast<std::size_t>(start_ + bytes_);
    from = std::max(from, start);
    to = std::min(to, end);
    if (from >= to) return 0;
    host::copy_mapped(data_ + (from - start), reinterpret_cast<const std::uint8_t*>(from), to - from, false);
    if (card_) card_->note_upload(from - start, to - from);
    return to - from;
}

void register_buffer(std::uintptr_t resource, std::uint64_t start, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_buffers_mutex);
    g_buffers.insert_or_assign(BufferKey{start, resource}, Entry{start + bytes, nullptr});
    g_buffers_generation.fetch_add(1, std::memory_order_relaxed);
}

void unregister_buffer(std::uintptr_t resource, std::uint64_t start) {
    std::shared_ptr<Shadow> dropped;
    {
        std::lock_guard<std::mutex> lock(g_buffers_mutex);
        const auto found = g_buffers.find(BufferKey{start, resource});
        if (found == g_buffers.end()) return;
        dropped = std::move(found->second.shadow);
        g_buffers.erase(found);
        g_buffers_generation.fetch_add(1, std::memory_order_relaxed);
    }
    if (dropped) host::retire(std::move(dropped));
}

std::optional<std::shared_ptr<Shadow>> find(std::uint64_t address, std::uint64_t bytes) {
    const std::uint64_t wanted = std::max(bytes, std::uint64_t{1});
    if (address > std::numeric_limits<std::uint64_t>::max() - wanted) return std::nullopt;
    const std::uint64_t end = address + wanted;
    BufferKey key{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_buffers_mutex);
        auto it = g_buffers.upper_bound(BufferKey{address, std::numeric_limits<std::uintptr_t>::max()});
        for (int step = 0; step < 64 && it != g_buffers.begin(); ++step) {
            --it;
            if (it->second.end >= end) {
                key = it->first;
                found = true;
                break;
            }
        }
        if (!found) {
            return loose(address, end);
        }
        Entry& entry = g_buffers[key];
        if (!entry.shadow) {
            const std::uint64_t len = entry.end - key.first;
            host::Host* gpu_host = host::host();
            if (gpu_host == nullptr) return std::nullopt;
            if (auto side = card::Side::try_create(gpu_host->device(), len)) {
                entry.shadow = std::make_shared<Shadow>(side->buffer(), side->staging(), side->gpu(), key.first, len);
                entry.shadow->bind_card(std::move(side));
            } else {
                const std::uint64_t rounded = (len + kCopyAlignment - 1) / kCopyAlignment * kCopyAlignment;
                auto buffer = gpu_host->shared_buffer(rounded);
                if (!buffer) return std::nullopt;
                host::name(buffer->first.Get(), "title buffer");
                const std::uint64_t gpu = buffer->first->GetGPUVirtualAddress();
                if (len >= (16 << 20))
                    G6SHIM_INFO("d3d12_xs: large title buffer %llx+%llx copied to the host",
                                static_cast<unsigned long long>(key.first), static_cast<unsigned long long>(len));
                entry.shadow = std::make_shared<Shadow>(std::move(buffer->first), buffer->second, gpu, key.first, len);
            }
            g_buffers_generation.fetch_add(1, std::memory_order_relaxed);
        }
        return entry.shadow;
    }
}

std::uint64_t buffers_generation() { return g_buffers_generation.load(std::memory_order_relaxed); }

std::vector<std::shared_ptr<Shadow>> addressable() {
    std::vector<std::shared_ptr<Shadow>> shadows;
    std::lock_guard<std::mutex> lock(g_buffers_mutex);
    for (const auto& [key, entry] : g_buffers)
        if (entry.shadow) shadows.push_back(entry.shadow);
    return shadows;
}

std::string nearest(std::uint64_t va) {
    struct Copy {
        std::uint64_t gpu;
        std::uint64_t start;
        std::uint64_t bytes;
    };
    std::vector<Copy> all;
    {
        std::lock_guard<std::mutex> lock(g_buffers_mutex);
        for (const auto& [key, entry] : g_buffers)
            if (entry.shadow)
                all.push_back(Copy{entry.shadow->gpu(key.first), key.first, entry.end - key.first});
    }
    {
        std::lock_guard<std::mutex> lock(g_loose_mutex);
        for (const auto& [key, shadow] : g_loose)
            all.push_back(Copy{shadow->gpu(key.first), key.first, shadow->bytes()});
    }
    const Copy* below = nullptr;
    const Copy* above = nullptr;
    for (const Copy& copy : all) {
        if (copy.gpu <= va && (below == nullptr || copy.gpu > below->gpu)) below = &copy;
        if (copy.gpu > va && (above == nullptr || copy.gpu < above->gpu)) above = &copy;
    }
    char text[256] = {};
    if (below != nullptr) {
        std::snprintf(text, sizeof text,
                      "; the nearest title memory copy below it: %llx+%llx at host %llx, offset %llx",
                      static_cast<unsigned long long>(below->start), static_cast<unsigned long long>(below->bytes),
                      static_cast<unsigned long long>(below->gpu),
                      static_cast<unsigned long long>(va - below->gpu));
    } else {
        std::snprintf(text, sizeof text, "; no title memory copy below it");
    }
    std::string out = text;
    if (above != nullptr) {
        std::snprintf(text, sizeof text, "; the nearest above: %llx+%llx at host %llx (%zu copies in all)",
                      static_cast<unsigned long long>(above->start), static_cast<unsigned long long>(above->bytes),
                      static_cast<unsigned long long>(above->gpu), all.size());
        out += text;
    }
    return out;
}

void collect_host_ranges(std::vector<HostRange>& out) {
    {
        std::lock_guard<std::mutex> lock(g_buffers_mutex);
        for (const auto& [key, entry] : g_buffers) {
            if (!entry.shadow) continue;
            HostRange range;
            range.gpu = entry.shadow->gpu(key.first);
            range.bytes = entry.end - key.first;
            range.label = "shadow";
            out.push_back(std::move(range));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_loose_mutex);
        for (const auto& [key, window] : g_loose) {
            if (!window) continue;
            HostRange range;
            range.gpu = window->gpu(key.first);
            range.bytes = window->bytes();
            range.label = "loose";
            out.push_back(std::move(range));
        }
    }
}

RamBand ram_band(RamBand previous, const RamSample& s) {
    constexpr std::uint64_t kGiB = 1ull << 30;
    const bool high = s.private_bytes * 5 >= s.commit_limit * 4 || s.working * 2 >= s.total_physical ||
                      s.free_physical < 2 * kGiB || s.commit_headroom < kGiB;
    const bool watch = s.private_bytes * 10 >= s.commit_limit * 7 || s.working * 4 >= s.total_physical ||
                       s.free_physical < 4 * kGiB || s.commit_headroom < 2 * kGiB;
    const RamBand wanted = high ? RamBand::kHigh : watch ? RamBand::kWatch : RamBand::kCalm;
    const bool safe_high = s.private_bytes * 20 <= s.commit_limit * 13 && s.working * 4 <= s.total_physical &&
                           s.free_physical >= 4 * kGiB && s.commit_headroom >= 3 * kGiB;
    const bool safe_watch = s.private_bytes * 5 <= s.commit_limit * 3 && s.working * 5 <= s.total_physical &&
                            s.free_physical >= 6 * kGiB && s.commit_headroom >= 3 * kGiB;
    if (previous == RamBand::kHigh && wanted == RamBand::kCalm && !safe_high) return RamBand::kWatch;
    if (previous == RamBand::kWatch && wanted == RamBand::kCalm && !safe_watch) return RamBand::kWatch;
    return wanted;
}

namespace {

constexpr std::uint64_t kRamSampleSubmissions = 32;
constexpr std::array<std::uint64_t, 3> kBandIdleSubmissions = {kIdleSubmissions, 128, 16};
constexpr std::array<std::uint64_t, 3> kBandLooseBytes = {kLooseBudgetBytes, 2ull << 30, 1ull << 30};

std::optional<RamSample> ram_sample() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (!::GlobalMemoryStatusEx(&status)) return std::nullopt;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!::K32GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                   sizeof counters))
        return std::nullopt;
    RamSample sample;
    sample.working = counters.WorkingSetSize;
    sample.private_bytes = counters.PrivateUsage;
    sample.free_physical = status.ullAvailPhys;
    sample.commit_headroom = status.ullAvailPageFile;
    sample.total_physical = status.ullTotalPhys;
    sample.commit_limit = status.ullTotalPageFile;
    return sample;
}

const char* band_name(RamBand band) {
    switch (band) {
    case RamBand::kCalm: return "calm";
    case RamBand::kWatch: return "watch";
    case RamBand::kHigh: return "high";
    }
    return "?";
}

std::atomic<RamBand> g_band{RamBand::kCalm};

RamBand band_now(std::uint64_t submission) {
    if (submission % kRamSampleSubmissions != 0) return g_band.load(std::memory_order_relaxed);
    const std::optional<RamSample> sample = ram_sample();
    if (!sample) return g_band.load(std::memory_order_relaxed);
    const RamBand previous = g_band.load(std::memory_order_relaxed);
    const RamBand band = ram_band(previous, *sample);
    if (band != previous) {
        g_band.store(band, std::memory_order_relaxed);
        G6SHIM_INFO("d3d12_xs: memory pressure %s (%llu MiB free, %llu MiB private, %llu MiB commit left)",
                    band_name(band), static_cast<unsigned long long>(sample->free_physical >> 20),
                    static_cast<unsigned long long>(sample->private_bytes >> 20),
                    static_cast<unsigned long long>(sample->commit_headroom >> 20));
    }
    return band;
}

}  // namespace

void submission_completed() {
    const std::uint64_t now = g_submission.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto band = static_cast<std::size_t>(band_now(now));
    const std::uint64_t idle = kBandIdleSubmissions[band];
    const std::uint64_t budget = std::min(g_loose_budget.load(std::memory_order_relaxed), kBandLooseBytes[band]);
    std::vector<std::shared_ptr<Shadow>> gone;
    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(g_loose_mutex);
        const bool flying = host::submission_in_flight();
        std::uint64_t held = 0;
        for (const auto& [key, window] : g_loose)
            if (window) held += window->bytes();
        const bool over_budget = held > budget;
        for (auto it = g_loose.begin(); over_budget && it != g_loose.end();) {
            const std::shared_ptr<Shadow>& window = it->second;
            if (!window || window.use_count() != 1 || now - window->last_used() <= idle ||
                (flying && host::references(window->buffer()) > 1)) {
                ++it;
                continue;
            }
            g_loose_buffers.erase(window->buffer());
            gone.push_back(window);
            it = g_loose.erase(it);
            g_evicted.fetch_add(1, std::memory_order_relaxed);
            evicted = true;
        }
    }
    if (evicted) g_generation.fetch_add(1, std::memory_order_release);
    for (std::shared_ptr<Shadow>& shadow : gone) host::retire(std::move(shadow));
}

std::uint64_t eviction_generation() { return g_generation.load(std::memory_order_acquire); }

bool live_loose(ID3D12Resource* buffer) {
    std::lock_guard<std::mutex> lock(g_loose_mutex);
    return g_loose_buffers.contains(buffer);
}

void set_loose_budget_for_test(std::uint64_t bytes) { g_loose_budget.store(bytes, std::memory_order_relaxed); }

BufferStats buffer_stats() {
    std::lock_guard<std::mutex> lock(g_buffers_mutex);
    BufferStats stats;
    for (const auto& [key, entry] : g_buffers) {
        if (!entry.shadow) continue;
        const std::uint64_t bytes = entry.end - key.first;
        ++stats.buffers;
        stats.bytes += bytes;
        if (!entry.shadow->on_card()) continue;
        ++stats.large;
        stats.large_bytes += bytes;
    }
    return stats;
}

LooseStats loose_stats() {
    std::lock_guard<std::mutex> lock(g_loose_mutex);
    LooseStats stats;
    stats.windows = g_loose.size();
    for (const auto& [key, window] : g_loose) stats.bytes += window->bytes();
    stats.evicted = g_evicted.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace g6shim::d3d12_xs::shadow
