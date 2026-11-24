// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/title_memory.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace {

std::vector<std::pair<const void*, std::size_t>> g_seen;

void record(const void* at, std::size_t bytes) { g_seen.emplace_back(at, bytes); }

}  // namespace

TEST(title_memory_remaps_reach_the_registered_observer) {
    g_seen.clear();
    g6shim::title_memory_remapped(nullptr, 0);
    CHECK(g_seen.empty());
    g6shim::set_title_memory_remap_observer(&record);
    g6shim::title_memory_remapped(g_seen.data(), 64);
    g6shim::set_title_memory_remap_observer(nullptr);
    g6shim::title_memory_remapped(nullptr, 0);
    CHECK(g_seen.size() == 1);
    CHECK(g_seen[0].second == 64);
}
