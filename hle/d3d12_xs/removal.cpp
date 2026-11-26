// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "removal.hpp"

#include "card.hpp"
#include "descriptor_host.hpp"
#include "shadow.hpp"
#include "texture.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace g6shim::d3d12_xs::removal {

namespace {

std::string utf8_from_wide(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::string allocation_name(const char* narrow, const wchar_t* wide) {
    if (wide != nullptr && wide[0] != L'\0') return utf8_from_wide(wide);
    if (narrow != nullptr && narrow[0] != '\0') return narrow;
    return "unnamed";
}

std::string list_name(const char* narrow, const wchar_t* wide, const void* list) {
    const std::string named = allocation_name(narrow, wide);
    if (named != "unnamed") return named;
    char fallback[32] = {};
    std::snprintf(fallback, sizeof fallback, "%p", list);
    return fallback;
}

std::string context_at(const UnfinishedList& list, std::uint32_t index) {
    std::string found;
    for (const BreadcrumbContext& context : list.contexts)
        if (context.index == index) found = context.text;
    return found;
}

void log_once_unavailable(const char* text) {
    static std::once_flag once;
    std::call_once(once, [text] { G6SHIM_WARN("d3d12_xs: %s", text); });
}

void log_allocations(const char* kind, const D3D12_DRED_ALLOCATION_NODE1* node) {
    std::uint32_t logged = 0;
    for (; node != nullptr && logged < kMaxLoggedAllocations; node = node->pNext, ++logged)
        G6SHIM_ERROR("d3d12_xs: %s allocation %s", kind, allocation_name(node->ObjectNameA, node->ObjectNameW).c_str());
}

void log_allocations(const char* kind, const D3D12_DRED_ALLOCATION_NODE* node) {
    std::uint32_t logged = 0;
    for (; node != nullptr && logged < kMaxLoggedAllocations; node = node->pNext, ++logged)
        G6SHIM_ERROR("d3d12_xs: %s allocation %s", kind, allocation_name(node->ObjectNameA, node->ObjectNameW).c_str());
}

void log_report(const BreadcrumbReport& report) {
    G6SHIM_ERROR("d3d12_xs: list %s stopped after %u of %u", report.name.c_str(), report.done, report.total);
    for (const BreadcrumbStep& step : report.steps) {
        if (step.context.empty())
            G6SHIM_ERROR("d3d12_xs:   op %u %s", step.index, step.op_name.c_str());
        else
            G6SHIM_ERROR("d3d12_xs:   op %u %s %s", step.index, step.op_name.c_str(), step.context.c_str());
    }
}

void log_span(const char* which, const std::optional<GpuSpan>& span) {
    if (!span) {
        G6SHIM_ERROR("d3d12_xs: nearest %s: none", which);
        return;
    }
    G6SHIM_ERROR("d3d12_xs: nearest %s: %s %llx+%llx", which, span->label.c_str(),
                 static_cast<unsigned long long>(span->gpu), static_cast<unsigned long long>(span->bytes));
}

void log_nearest(std::uint64_t address) {
    G6SHIM_ERROR("d3d12_xs: nearest search for 0x%llX", static_cast<unsigned long long>(address));
    std::vector<GpuSpan> spans;
    std::vector<shadow::HostRange> title;
    shadow::collect_host_ranges(title);
    spans.reserve(title.size());
    for (const shadow::HostRange& range : title)
        spans.push_back(GpuSpan{range.gpu, range.bytes, range.label});
    for (const card::GpuRange& range : card::live_ranges())
        spans.push_back(GpuSpan{range.gpu, range.bytes, "card buffer"});
    for (const texture::GpuRange& range : texture::live_ranges())
        spans.push_back(GpuSpan{range.gpu, range.bytes, range.name});
    std::vector<descriptor_host::ShaderHeapRange> heaps;
    descriptor_host::collect_shader_heaps(heaps);
    for (const descriptor_host::ShaderHeapRange& heap : heaps)
        spans.push_back(GpuSpan{heap.gpu, heap.bytes, heap.sampler ? "sampler heap" : "view heap"});
    if (spans.empty()) G6SHIM_ERROR("d3d12_xs: nearest: none tracked");
    const NearestSpans nearest = nearest_spans(address, spans);
    if (nearest.containing)
        G6SHIM_ERROR("d3d12_xs: page fault inside %s %llx+%llx", nearest.containing->label.c_str(),
                     static_cast<unsigned long long>(nearest.containing->gpu),
                     static_cast<unsigned long long>(nearest.containing->bytes));
    log_span("below", nearest.below);
    log_span("above", nearest.above);
    const std::string heap_text = descriptor_host::near_heaps(address);
    if (heap_text.empty())
        G6SHIM_ERROR("d3d12_xs: descriptor heaps: none tracked");
    else
        G6SHIM_ERROR("d3d12_xs: descriptor heaps%s", heap_text.c_str());
}

void log_markers(const UnfinishedList& list) {
    const std::vector<std::string> recorded = markers_for(list.name);
    if (recorded.empty()) return;
    const std::vector<RecordedMarker> around = markers_around(list.ops, list.done, recorded);
    for (const RecordedMarker& marker : around) {
        if (marker.text.empty())
            G6SHIM_ERROR("d3d12_xs:   marker op %u", marker.index);
        else
            G6SHIM_ERROR("d3d12_xs:   marker op %u %s", marker.index, marker.text.c_str());
    }
}

void log_node(const char* narrow, const wchar_t* wide, const void* list, std::uint32_t done, std::uint32_t total,
              const D3D12_AUTO_BREADCRUMB_OP* history, const D3D12_DRED_BREADCRUMB_CONTEXT* contexts,
              std::uint32_t context_count) {
    if (done >= total) return;
    UnfinishedList unfinished;
    unfinished.name = list_name(narrow, wide, list);
    unfinished.done = done;
    unfinished.total = total;
    unfinished.ops.reserve(total);
    for (std::uint32_t i = 0; i < total && history != nullptr; ++i)
        unfinished.ops.push_back(static_cast<std::uint32_t>(history[i]));
    if (contexts != nullptr) {
        for (std::uint32_t i = 0; i < context_count; ++i) {
            BreadcrumbContext context;
            context.index = contexts[i].BreadcrumbIndex;
            context.text = utf8_from_wide(contexts[i].pContextString);
            unfinished.contexts.push_back(std::move(context));
        }
    }
    if (const std::optional<BreadcrumbReport> report = format_unfinished_list(unfinished)) {
        log_report(*report);
        log_markers(unfinished);
    }
}

void log_page_fault(ID3D12DeviceRemovedExtendedData* dred) {
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> newer;
    if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&newer)))) {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        if (FAILED(newer->GetPageFaultAllocationOutput1(&fault))) {
            G6SHIM_ERROR("d3d12_xs: page fault allocations unavailable");
            return;
        }
        G6SHIM_ERROR("d3d12_xs: page fault at 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
        log_nearest(fault.PageFaultVA);
        log_allocations("existing", fault.pHeadExistingAllocationNode);
        log_allocations("freed", fault.pHeadRecentFreedAllocationNode);
        return;
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (FAILED(dred->GetPageFaultAllocationOutput(&fault))) {
        G6SHIM_ERROR("d3d12_xs: page fault allocations unavailable");
        return;
    }
    G6SHIM_ERROR("d3d12_xs: page fault at 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
    log_nearest(fault.PageFaultVA);
    log_allocations("existing", fault.pHeadExistingAllocationNode);
    log_allocations("freed", fault.pHeadRecentFreedAllocationNode);
}

void log_breadcrumbs(ID3D12DeviceRemovedExtendedData* dred) {
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> newer;
    if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&newer)))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
        if (FAILED(newer->GetAutoBreadcrumbsOutput1(&crumbs))) {
            G6SHIM_ERROR("d3d12_xs: auto-breadcrumbs unavailable");
            return;
        }
        if (crumbs.pHeadAutoBreadcrumbNode == nullptr) {
            G6SHIM_ERROR("d3d12_xs: no breadcrumb nodes");
            return;
        }
        std::uint32_t logged = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode;
             node != nullptr && logged < kMaxLoggedLists; node = node->pNext) {
            const std::uint32_t done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (!list_needs_breadcrumb_log(done, node->BreadcrumbCount)) continue;
            ++logged;
            if (node->BreadcrumbCount == 0) {
                G6SHIM_ERROR("d3d12_xs: list %s has no breadcrumbs",
                             list_name(node->pCommandListDebugNameA, node->pCommandListDebugNameW, node->pCommandList)
                                 .c_str());
                continue;
            }
            log_node(node->pCommandListDebugNameA, node->pCommandListDebugNameW, node->pCommandList, done,
                     node->BreadcrumbCount, node->pCommandHistory, node->pBreadcrumbContexts,
                     node->BreadcrumbContextsCount);
        }
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
        G6SHIM_ERROR("d3d12_xs: auto-breadcrumbs unavailable");
        return;
    }
    if (crumbs.pHeadAutoBreadcrumbNode == nullptr) {
        G6SHIM_ERROR("d3d12_xs: no breadcrumb nodes");
        return;
    }
    std::uint32_t logged = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode;
         node != nullptr && logged < kMaxLoggedLists; node = node->pNext) {
        const std::uint32_t done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
        if (!list_needs_breadcrumb_log(done, node->BreadcrumbCount)) continue;
        ++logged;
        if (node->BreadcrumbCount == 0) {
            G6SHIM_ERROR("d3d12_xs: list %s has no breadcrumbs",
                         list_name(node->pCommandListDebugNameA, node->pCommandListDebugNameW, node->pCommandList)
                             .c_str());
            continue;
        }
        log_node(node->pCommandListDebugNameA, node->pCommandListDebugNameW, node->pCommandList, done,
                 node->BreadcrumbCount, node->pCommandHistory, nullptr, 0);
    }
}

}  // namespace

std::string breadcrumb_op_name(std::uint32_t op) {
    switch (op) {
    case 0: return "SetMarker";
    case 1: return "BeginEvent";
    case 2: return "EndEvent";
    case 3: return "DrawInstanced";
    case 4: return "DrawIndexedInstanced";
    case 5: return "ExecuteIndirect";
    case 6: return "Dispatch";
    case 7: return "CopyBufferRegion";
    case 8: return "CopyTextureRegion";
    case 9: return "CopyResource";
    case 12: return "ClearRenderTargetView";
    case 13: return "ClearUnorderedAccessView";
    case 14: return "ClearDepthStencilView";
    case 15: return "ResourceBarrier";
    case 17: return "Present";
    case 18: return "ResolveQueryData";
    case 31: return "BuildRaytracingAccelerationStructure";
    case 34: return "DispatchRays";
    case 42: return "DispatchMesh";
    default: return "op " + std::to_string(op);
    }
}

std::optional<BreadcrumbReport> format_unfinished_list(const UnfinishedList& list) {
    if (list.total == 0 || list.done >= list.total) return std::nullopt;
    const bool started = list.done > 0;
    const std::uint32_t last = started ? list.done - 1 : 0;
    const std::uint32_t from = started && last > kOpsBeforeLastCompleted ? last - kOpsBeforeLastCompleted : 0;
    const std::uint32_t after = started ? last + kOpsAfterLastCompleted : kOpsAfterLastCompleted;
    const std::uint32_t to = std::min(list.total - 1, after);
    BreadcrumbReport report;
    report.name = list.name.empty() ? "unnamed" : list.name;
    report.done = list.done;
    report.total = list.total;
    if (from > to) return report;
    for (std::uint32_t index = from; index <= to; ++index) {
        if (index >= list.ops.size()) break;
        BreadcrumbStep step;
        step.index = index;
        step.op = list.ops[index];
        step.op_name = breadcrumb_op_name(step.op);
        step.context = context_at(list, index);
        report.steps.push_back(std::move(step));
    }
    return report;
}

namespace {

std::mutex marker_mutex;
std::unordered_map<std::string, std::vector<std::string>> live_markers;
std::deque<std::pair<std::string, std::vector<std::string>>> retired_markers;
std::unordered_map<void*, std::string> list_names;

}  // namespace

std::vector<RecordedMarker> markers_around(std::span<const std::uint32_t> ops, std::uint32_t done,
                                           std::span<const std::string> recorded) {
    std::vector<RecordedMarker> placed;
    std::size_t next = 0;
    for (std::uint32_t index = 0; index < ops.size(); ++index) {
        if (ops[index] != kSetMarker) continue;
        RecordedMarker marker;
        marker.index = index;
        if (next < recorded.size()) marker.text = recorded[next];
        ++next;
        placed.push_back(std::move(marker));
    }
    const bool started = done > 0;
    const std::uint32_t last = started ? done - 1 : 0;
    std::vector<RecordedMarker> before;
    std::vector<RecordedMarker> after;
    for (const RecordedMarker& marker : placed) {
        if (started && marker.index <= last)
            before.push_back(marker);
        else
            after.push_back(marker);
    }
    if (before.size() > kMarkersBeforeLast)
        before.erase(before.begin(), before.end() - static_cast<std::ptrdiff_t>(kMarkersBeforeLast));
    if (after.size() > kMarkersAfterLast) after.resize(kMarkersAfterLast);
    before.insert(before.end(), after.begin(), after.end());
    return before;
}

NearestSpans nearest_spans(std::uint64_t address, std::span<const GpuSpan> spans) {
    NearestSpans found;
    std::uint64_t below_end = 0;
    std::uint64_t above_start = std::numeric_limits<std::uint64_t>::max();
    for (const GpuSpan& span : spans) {
        if (span.bytes == 0 || span.gpu > std::numeric_limits<std::uint64_t>::max() - span.bytes) continue;
        const std::uint64_t end = span.gpu + span.bytes;
        if (span.gpu <= address && address < end) {
            found.containing = span;
            continue;
        }
        if (end <= address && (found.below == std::nullopt || end >= below_end)) {
            below_end = end;
            found.below = span;
        }
        if (span.gpu > address && span.gpu < above_start) {
            above_start = span.gpu;
            found.above = span;
        }
    }
    return found;
}

bool list_needs_breadcrumb_log(std::uint32_t done, std::uint32_t total) { return total == 0 || done < total; }

void name_list(void* list_pointer, std::string_view label) {
    if (list_pointer == nullptr || label.empty()) return;
    auto* unknown = static_cast<IUnknown*>(list_pointer);
    Microsoft::WRL::ComPtr<ID3D12Object> named;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&named)))) return;
    const std::string text(label.substr(0, kMaxMarkerChars));
    std::wstring wide(text.size(), L'\0');
    if (!text.empty())
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                              static_cast<int>(wide.size()));
    named->SetName(wide.c_str());
    std::lock_guard<std::mutex> lock(marker_mutex);
    list_names[list_pointer] = text;
}

void note_list_marker(std::string_view list_name, std::string text) {
    if (list_name.empty() || !requested()) return;
    if (text.size() > kMaxMarkerChars) text.resize(kMaxMarkerChars);
    std::lock_guard<std::mutex> lock(marker_mutex);
    live_markers[std::string(list_name)].push_back(std::move(text));
}

std::vector<std::string> markers_for(std::string_view list_name) {
    std::lock_guard<std::mutex> lock(marker_mutex);
    const auto live = live_markers.find(std::string(list_name));
    if (live != live_markers.end()) return live->second;
    for (auto retired = retired_markers.rbegin(); retired != retired_markers.rend(); ++retired)
        if (retired->first == list_name) return retired->second;
    return {};
}

void forget_markers(std::string_view list_name) {
    if (list_name.empty()) return;
    std::lock_guard<std::mutex> lock(marker_mutex);
    const auto live = live_markers.find(std::string(list_name));
    if (live == live_markers.end()) return;
    retired_markers.emplace_back(live->first, std::move(live->second));
    live_markers.erase(live);
    while (retired_markers.size() > kRetiredMarkerLists) retired_markers.pop_front();
}

bool requested() { return settings::enabled("G6SHIM_D3D_DRED"); }

void enable() {
    if (!requested()) return;
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (FAILED(::D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
        log_once_unavailable("removal diagnostics not available");
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> contexts;
    if (FAILED(settings.As(&contexts))) {
        log_once_unavailable("removal breadcrumb contexts not available");
        return;
    }
    contexts->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}

void report(void* device_pointer) {
    auto* device = static_cast<ID3D12Device*>(device_pointer);
    if (device == nullptr) return;
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return;
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    G6SHIM_ERROR("d3d12_xs: device removed 0x%08lX", static_cast<unsigned long>(reason));
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return;
    log_page_fault(dred.Get());
    log_breadcrumbs(dred.Get());
}

void mark(void* list_pointer, std::string_view label) {
    if (!requested() || list_pointer == nullptr) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(list_pointer);
    const std::string text(label.empty() ? std::string_view("untitled") : label.substr(0, kMaxMarkerChars));
    std::wstring wide(text.size(), L'\0');
    if (!text.empty())
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                              static_cast<int>(wide.size()));
    list->SetMarker(0, wide.c_str(), static_cast<UINT>((wide.size() + 1) * sizeof(wchar_t)));
    std::string named;
    {
        std::lock_guard<std::mutex> lock(marker_mutex);
        const auto found = list_names.find(list_pointer);
        if (found != list_names.end()) named = found->second;
    }
    if (!named.empty()) note_list_marker(named, text);
}

}  // namespace g6shim::d3d12_xs::removal
