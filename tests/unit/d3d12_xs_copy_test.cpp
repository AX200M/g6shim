// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/watch.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

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
