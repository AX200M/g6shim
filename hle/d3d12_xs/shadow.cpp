// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shadow.hpp"

#include "card.hpp"
#include "watch.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
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
std::atomic<std::uint64_t> g_evicted{0};
std::atomic<std::uint64_t> g_loose_budget{kLooseBudgetBytes};

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
            host::write_mapped(reinterpret_cast<std::uint8_t*>(start + lo), title.data(), n);
            written += n;
        }
        if (kept) card_->note_kept(static_cast<std::uint32_t>(piece), lo, n);
    }
    return written;
}

std::size_t Shadow::copy_back(std::size_t address, std::size_t bytes) {
    std::size_t written = 0;
    {
        std::lock_guard<std::mutex> snapshot_lock(before_mutex_);
        const auto [from, to] = inside(address, bytes);
        const std::size_t start = static_cast<std::size_t>(start_);
        if (card_) {
            written = merge_card(address, bytes);
        } else if (const auto held = before_of(from, to)) {
            const std::vector<std::uint8_t>& before = *held;
            constexpr std::size_t kPiece = 64 << 10;
            std::vector<std::uint8_t> title(kPiece, 0);
            std::vector<std::size_t> synced;
            std::size_t at = from;
            while (at < to) {
                const std::size_t n = std::min(kPiece - (start + at) % kPiece, to - at);
                std::uint8_t* host = data_ + at;
                const std::uint8_t* old = before.data() + (at - from);
                if (n == 0 || host == nullptr || old == nullptr) break;
                if (host::read_into(reinterpret_cast<const std::uint8_t*>(start + at),
                                    std::span<std::uint8_t>(title.data(), n))) {
                    bool changed = false;
                    for (std::size_t i = 0; i < n; ++i) {
                        if (title[i] != old[i]) {
                            host[i] = title[i];
                        } else if (host[i] != old[i]) {
                            title[i] = host[i];
                            changed = true;
                        }
                    }
                    if (changed) {
                        watch::writing(start + at, n);
                        host::write_mapped(reinterpret_cast<std::uint8_t*>(start + at), title.data(), n);
                        written += n;
                    }
                    if (n > 0 && host != nullptr && old != nullptr && (changed || std::memcmp(host, old, n) != 0))
                        synced.push_back((start + at) / watch::kPageBytes);
                }
                at += n;
            }
            const std::size_t first = (start + from + watch::kPageBytes - 1) / watch::kPageBytes;
            const std::size_t last = (start + to) / watch::kPageBytes;
            if (first < last && !synced.empty() && watched_.load(std::memory_order_relaxed) != kNotWatched) {
                const auto pages = watch::writes(first * watch::kPageBytes, last * watch::kPageBytes, true);
                if (pages) {
                    const std::size_t base = start / watch::kPageBytes;
                    std::lock_guard<std::mutex> lock(copied_mutex_);
                    for (const std::size_t page : synced)
                        if (page >= base && page - base < copied_.size() && page >= first && page < last)
                            copied_[page - base] = pages->asked;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto range = std::make_pair(address, address + bytes);
        pending_.erase(std::remove(pending_.begin(), pending_.end(), range), pending_.end());
        if (pending_.empty()) {
            std::lock_guard<std::mutex> snapshot_lock(before_mutex_);
            drop_before();
        }
    }
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
}

void unregister_buffer(std::uintptr_t resource, std::uint64_t start) {
    std::shared_ptr<Shadow> dropped;
    {
        std::lock_guard<std::mutex> lock(g_buffers_mutex);
        const auto found = g_buffers.find(BufferKey{start, resource});
        if (found == g_buffers.end()) return;
        dropped = std::move(found->second.shadow);
        g_buffers.erase(found);
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
        }
        return entry.shadow;
    }
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

void submission_completed() {
    const std::uint64_t now = g_submission.fetch_add(1, std::memory_order_relaxed) + 1;
    std::vector<std::shared_ptr<Shadow>> gone;
    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(g_loose_mutex);
        const bool flying = host::submission_in_flight();
        std::uint64_t held = 0;
        for (const auto& [key, window] : g_loose)
            if (window) held += window->bytes();
        const bool over_budget = held > g_loose_budget.load(std::memory_order_relaxed);
        for (auto it = g_loose.begin(); over_budget && it != g_loose.end();) {
            const std::shared_ptr<Shadow>& window = it->second;
            if (!window || window.use_count() != 1 || now - window->last_used() <= kIdleSubmissions ||
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
