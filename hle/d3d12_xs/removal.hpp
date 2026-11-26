// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::d3d12_xs::removal {

inline constexpr std::uint32_t kOpsBeforeLastCompleted = 4;
inline constexpr std::uint32_t kOpsAfterLastCompleted = 3;
inline constexpr std::uint32_t kMarkersBeforeLast = 3;
inline constexpr std::uint32_t kMarkersAfterLast = 2;
inline constexpr std::uint32_t kSetMarker = 0;
inline constexpr std::uint32_t kMaxLoggedLists = 16;
inline constexpr std::uint32_t kMaxLoggedAllocations = 8;
inline constexpr std::size_t kMaxMarkerChars = 160;
inline constexpr std::size_t kRetiredMarkerLists = 48;

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

struct RecordedMarker {
    std::uint32_t index = 0;
    std::string text;
};

std::vector<RecordedMarker> markers_around(std::span<const std::uint32_t> ops, std::uint32_t done,
                                           std::span<const std::string> recorded);

struct GpuSpan {
    std::uint64_t gpu = 0;
    std::uint64_t bytes = 0;
    std::string label;
};

struct NearestSpans {
    std::optional<GpuSpan> containing;
    std::optional<GpuSpan> below;
    std::optional<GpuSpan> above;
};

NearestSpans nearest_spans(std::uint64_t address, std::span<const GpuSpan> spans);

void note_list_marker(std::string_view list_name, std::string text);
std::vector<std::string> markers_for(std::string_view list_name);
void forget_markers(std::string_view list_name);

void enable();
void report(void* device);
bool requested();
void mark(void* list, std::string_view label);

}  // namespace g6shim::d3d12_xs::removal
