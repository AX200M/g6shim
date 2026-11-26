// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::d3d12_xs::removal {

inline constexpr std::uint32_t kOpsBeforeLastCompleted = 4;
inline constexpr std::uint32_t kOpsAfterLastCompleted = 3;
inline constexpr std::uint32_t kMaxLoggedLists = 16;
inline constexpr std::uint32_t kMaxLoggedAllocations = 8;
inline constexpr std::size_t kMaxMarkerChars = 160;

struct BreadcrumbContext {
    std::uint32_t index = 0;
    std::string text;
};

struct UnfinishedList {
    std::string name;
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    std::vector<std::uint32_t> ops;
    std::vector<BreadcrumbContext> contexts;
};

struct BreadcrumbStep {
    std::uint32_t index = 0;
    std::uint32_t op = 0;
    std::string op_name;
    std::string context;
};

struct BreadcrumbReport {
    std::string name;
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    std::vector<BreadcrumbStep> steps;
};

std::string breadcrumb_op_name(std::uint32_t op);
std::optional<BreadcrumbReport> format_unfinished_list(const UnfinishedList& list);

void enable();
void report(void* device);
bool requested();
void mark(void* list, std::string_view label);

}  // namespace g6shim::d3d12_xs::removal
