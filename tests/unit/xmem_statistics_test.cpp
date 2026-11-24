// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "statistics.hpp"

#include <cstddef>
#include <cstdint>

using namespace g6shim::xmem;

TEST(xmem_transfers_shrink_the_title_budget) {
    set_title_memory(kDefaultTitleMemory);
    WorkingSet before{};
    CHECK(get_working_set_statistics(0x2000'0001, &before) == 1);
    CHECK(before.title_used > 0);
    CHECK(transfer_memory(0, 128 * kMebibyte - 1) == 1);
    CHECK(transfer_memory(2, kMebibyte) == 0);
    CHECK(transfer_memory(0, title_memory().standard) == 0);

    WorkingSet after{};
    CHECK(get_working_set_statistics(0x2000'0001, &after) == 1);
    CHECK(before.title_limit - after.title_limit == 128 * kMebibyte);
}

TEST(xmem_statistics_have_the_console_layout) {
    CHECK(sizeof(WorkingSet) == std::size_t{0x50});
    CHECK(sizeof(BasicInformation) == std::size_t{0x40});
    CHECK(offsetof(WorkingSet, gpu_optimal_limit) == std::size_t{0x28});
    CHECK(offsetof(WorkingSet, kernel_pool_used) == std::size_t{0x48});

    WorkingSet stats{};
    CHECK(get_working_set_statistics(0, &stats) == 1);
    CHECK(stats.gpu_optimal_limit == title_memory().gpu_optimal);
    CHECK(stats.gpu_optimal_used == 0);
    CHECK(stats.tools_limit == 0);
    CHECK(get_working_set_statistics(0, nullptr) == 0);
}
