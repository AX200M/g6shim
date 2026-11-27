// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/shadow.hpp"
#include "d3d12_xs/watch.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace g6shim::d3d12_xs;

TEST(d3d12_xs_title_reads_copy_readable_memory_and_refuse_unreadable_pages) {
    constexpr std::size_t kPage = 4096;
    auto* memory = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 2 * kPage, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    CHECK(::VirtualAlloc(memory, kPage, MEM_COMMIT, PAGE_READWRITE) == memory);
    for (std::size_t i = 0; i < kPage; ++i) memory[i] = static_cast<std::uint8_t>(i * 13);

    std::array<std::uint8_t, 256> into{};
    CHECK(host::read_into(memory + 100, std::span<std::uint8_t>(into)));
    CHECK(std::memcmp(into.data(), memory + 100, into.size()) == 0);

    std::array<std::uint8_t, 64> missing{};
    CHECK(!host::read_into(memory + kPage, std::span<std::uint8_t>(missing)));
    CHECK(!host::read_into(memory + kPage, std::span<std::uint8_t>(missing)));

    std::array<std::uint8_t, 2 * 4096> spanning{};
    host::copy_mapped(spanning.data(), memory, spanning.size(), false);
    CHECK(std::memcmp(spanning.data(), memory, kPage) == 0);
    bool zeroed = true;
    for (std::size_t i = kPage; i < spanning.size(); ++i) zeroed = zeroed && spanning[i] == 0;
    CHECK(zeroed);
    ::VirtualFree(memory, 0, MEM_RELEASE);
}

TEST(d3d12_xs_unreadable_pages_are_forgotten_after_a_remap) {
    constexpr std::size_t kPage = host::kUnreadablePageBytes;
    void* memory = ::VirtualAlloc(nullptr, kPage, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const auto at = reinterpret_cast<std::size_t>(memory);
    host::UnreadablePages pages;
    CHECK(!pages.known(at, 16, 7));
    pages.note(at, 16, 7);
    CHECK(pages.known(at + 8, 4, 7));
    CHECK(!pages.known(at, 16, 8));
    pages.note(at + kPage, 16, 8);
    CHECK(!pages.known(at, 16, 8));
    ::VirtualFree(memory, 0, MEM_RELEASE);
}

TEST(d3d12_xs_write_merge_matches_the_byte_rule) {
    constexpr std::size_t kBytes = 4096 + 17;
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 24;
    };
    for (int round = 0; round < 64; ++round) {
        std::vector<std::uint8_t> old(kBytes);
        for (auto& byte : old) byte = static_cast<std::uint8_t>(next());
        std::vector<std::uint8_t> title = old;
        std::vector<std::uint8_t> host = old;
        const std::uint32_t density = 1 + static_cast<std::uint32_t>(round % 8) * 32;
        for (std::size_t i = 0; i < kBytes; ++i) {
            if (next() < density / 2) title[i] = static_cast<std::uint8_t>(next());
            if (next() < density / 2) host[i] = static_cast<std::uint8_t>(next());
        }
        std::vector<std::uint8_t> want_title = title;
        std::vector<std::uint8_t> want_host = host;
        bool want_changed = false;
        for (std::size_t i = 0; i < kBytes; ++i) {
            if (want_title[i] != old[i]) {
                want_host[i] = want_title[i];
            } else if (want_host[i] != old[i]) {
                want_title[i] = want_host[i];
                want_changed = true;
            }
        }
        const bool changed = shadow::merge_written(title.data(), host.data(), old.data(), kBytes);
        CHECK(changed == want_changed);
        CHECK(title == want_title);
        CHECK(host == want_host);
    }
}
