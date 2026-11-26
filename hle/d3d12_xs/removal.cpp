// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "removal.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>

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
    if (const std::optional<BreadcrumbReport> report = format_unfinished_list(unfinished)) log_report(*report);
}

void log_page_fault(ID3D12DeviceRemovedExtendedData* dred) {
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> newer;
    if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&newer)))) {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        if (FAILED(newer->GetPageFaultAllocationOutput1(&fault))) return;
        G6SHIM_ERROR("d3d12_xs: page fault at 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
        log_allocations("existing", fault.pHeadExistingAllocationNode);
        log_allocations("freed", fault.pHeadRecentFreedAllocationNode);
        return;
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (FAILED(dred->GetPageFaultAllocationOutput(&fault))) return;
    G6SHIM_ERROR("d3d12_xs: page fault at 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
    log_allocations("existing", fault.pHeadExistingAllocationNode);
    log_allocations("freed", fault.pHeadRecentFreedAllocationNode);
}

void log_breadcrumbs(ID3D12DeviceRemovedExtendedData* dred) {
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> newer;
    if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&newer)))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
        if (FAILED(newer->GetAutoBreadcrumbsOutput1(&crumbs))) return;
        std::uint32_t logged = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode;
             node != nullptr && logged < kMaxLoggedLists; node = node->pNext) {
            const std::uint32_t done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount) continue;
            ++logged;
            log_node(node->pCommandListDebugNameA, node->pCommandListDebugNameW, node->pCommandList, done,
                     node->BreadcrumbCount, node->pCommandHistory, node->pBreadcrumbContexts,
                     node->BreadcrumbContextsCount);
        }
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs))) return;
    std::uint32_t logged = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode;
         node != nullptr && logged < kMaxLoggedLists; node = node->pNext) {
        const std::uint32_t done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
        if (done >= node->BreadcrumbCount) continue;
        ++logged;
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
}

}  // namespace g6shim::d3d12_xs::removal
