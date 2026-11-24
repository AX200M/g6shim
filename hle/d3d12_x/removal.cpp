// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "removal.hpp"

#include <g6shim/log.hpp>

#include <atomic>
#include <cstdlib>

namespace g6shim::d3d12 {

namespace {

constexpr UINT kLoggedBreadcrumbNodes = 16;
constexpr UINT kLoggedOpsAfterLast = 4;

bool dred_requested() {
    const char* value = std::getenv("G6SHIM_D3D_DRED");
    return value && *value && *value != '0';
}

void log_page_fault(ID3D12DeviceRemovedExtendedData* dred) {
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (FAILED(dred->GetPageFaultAllocationOutput(&fault))) return;
    G6SHIM_ERROR("device removal: page fault at GPU address 0x%llX", static_cast<unsigned long long>(fault.PageFaultVA));
    for (const D3D12_DRED_ALLOCATION_NODE* node = fault.pHeadExistingAllocationNode; node; node = node->pNext)
        G6SHIM_ERROR("  allocation at the address: type %d", node->AllocationType);
    for (const D3D12_DRED_ALLOCATION_NODE* node = fault.pHeadRecentFreedAllocationNode; node; node = node->pNext)
        G6SHIM_ERROR("  allocation freed recently at the address: type %d", node->AllocationType);
}

void log_breadcrumbs(ID3D12DeviceRemovedExtendedData* dred) {
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs))) return;
    UINT logged = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node && logged < kLoggedBreadcrumbNodes;
         node = node->pNext) {
        const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (done >= node->BreadcrumbCount) continue;
        ++logged;
        G6SHIM_ERROR("device removal: list %p on queue %p stopped after %u of %u operations", node->pCommandList,
                     node->pCommandQueue, done, node->BreadcrumbCount);
        for (UINT i = done; i < node->BreadcrumbCount && i < done + kLoggedOpsAfterLast; ++i)
            G6SHIM_ERROR("  operation %u: %d", i, node->pCommandHistory[i]);
    }
}

}  // namespace

void enable_removal_diagnostics() {
    if (!dred_requested()) return;
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
        G6SHIM_WARN("G6SHIM_D3D_DRED: removal diagnostics not available");
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->Release();
}

bool report_device_removal(ID3D12Device* device) {
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (reason == S_OK) return false;
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return true;
    G6SHIM_ERROR("device removed: reason 0x%08lX", reason);
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        log_page_fault(dred);
        log_breadcrumbs(dred);
        dred->Release();
    }
    return true;
}

}  // namespace g6shim::d3d12
