// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "console_objects.hpp"

#include <g6shim/log.hpp>
#include <g6shim/window.hpp>

#include <dxgi1_4.h>

#include <array>
#include <atomic>
#include <map>
#include <mutex>

namespace g6shim::d3d12 {

namespace {

constexpr UINT kSwapChainBuffers = 3;
constexpr UINT kPresentSyncInterval = 0;
constexpr int kLoggedPresents = 4;

struct WindowSearch {
    DWORD process;
    HWND best;
    LONGLONG best_area;
};

BOOL CALLBACK consider_window(HWND window, LPARAM context) {
    auto* search = reinterpret_cast<WindowSearch*>(context);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search->process || GetWindow(window, GW_OWNER)) return TRUE;
    RECT rect{};
    GetClientRect(window, &rect);
    const LONGLONG area = LONGLONG{rect.right - rect.left} * (rect.bottom - rect.top);
    if (!search->best || area > search->best_area) {
        search->best = window;
        search->best_area = area;
    }
    return TRUE;
}

HWND title_window() {
    if (auto* window = static_cast<HWND>(main_window())) return window;
    WindowSearch search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(&consider_window, reinterpret_cast<LPARAM>(&search));
    if (search.best) set_main_window(search.best);
    return search.best;
}

DXGI_FORMAT swap_chain_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

struct Presenter {
    IDXGISwapChain3* swap_chain = nullptr;
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<ID3D12CommandAllocator*, kSwapChainBuffers> allocators{};
    std::array<UINT64, kSwapChainBuffers> done_at{};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 presented = 0;
};

struct Presenters {
    std::mutex mutex;
    std::map<ID3D12CommandQueue*, Presenter> by_queue;
};

Presenters& presenters() {
    static Presenters* p = new Presenters;
    return *p;
}

HRESULT create_swap_chain(ConsoleQueue* queue, const D3D12_RESOURCE_DESC& source, Presenter& presenter) {
    const HWND window = title_window();
    if (!window) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(source.Width);
    desc.Height = source.Height;
    desc.Format = swap_chain_format(source.Format);
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kSwapChainBuffers;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap_chain = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue->pc, window, &desc, nullptr, nullptr, &swap_chain);
    if (SUCCEEDED(hr)) {
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        hr = swap_chain->QueryInterface(IID_PPV_ARGS(&presenter.swap_chain));
        swap_chain->Release();
    }
    factory->Release();
    if (FAILED(hr)) return hr;
    presenter.width = source.Width;
    presenter.height = source.Height;
    presenter.format = source.Format;
    G6SHIM_INFO("PresentX: swap chain %llux%u, format %d, on window %p", source.Width, source.Height, desc.Format,
                window);
    return S_OK;
}

HRESULT prepare(ConsoleQueue* queue, const D3D12_RESOURCE_DESC& source, Presenter& presenter) {
    ID3D12Device* device = queue->device->pc;
    if (!presenter.fence) {
        for (auto& allocator : presenter.allocators)
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                return E_OUTOFMEMORY;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presenter.allocators[0], nullptr,
                                             IID_PPV_ARGS(&presenter.list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenter.fence))))
            return E_OUTOFMEMORY;
        presenter.list->Close();
    }
    if (!presenter.swap_chain) return create_swap_chain(queue, source, presenter);
    if (presenter.width == source.Width && presenter.height == source.Height && presenter.format == source.Format)
        return S_OK;
    presenter.fence->SetEventOnCompletion(presenter.presented, nullptr);
    const HRESULT hr = presenter.swap_chain->ResizeBuffers(kSwapChainBuffers, static_cast<UINT>(source.Width),
                                                           source.Height, swap_chain_format(source.Format), 0);
    if (SUCCEEDED(hr)) {
        presenter.width = source.Width;
        presenter.height = source.Height;
        presenter.format = source.Format;
    }
    return hr;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}

HRESULT present(ConsoleQueue* queue, ID3D12Resource* source, Presenter& presenter) {
    const D3D12_RESOURCE_DESC desc = source->GetDesc();
    HRESULT hr = prepare(queue, desc, presenter);
    if (FAILED(hr)) return hr;
    const UINT index = presenter.swap_chain->GetCurrentBackBufferIndex();
    ID3D12Resource* target = nullptr;
    hr = presenter.swap_chain->GetBuffer(index, IID_PPV_ARGS(&target));
    if (FAILED(hr)) return hr;
    ID3D12CommandAllocator* allocator = presenter.allocators[index];
    if (presenter.fence->GetCompletedValue() < presenter.done_at[index])
        presenter.fence->SetEventOnCompletion(presenter.done_at[index], nullptr);
    allocator->Reset();
    presenter.list->Reset(allocator, nullptr);
    const D3D12_RESOURCE_BARRIER before = transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    presenter.list->ResourceBarrier(1, &before);
    presenter.list->CopyResource(target, source);
    const D3D12_RESOURCE_BARRIER after = transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    presenter.list->ResourceBarrier(1, &after);
    presenter.list->Close();
    ID3D12CommandList* lists[] = {presenter.list};
    queue->pc->ExecuteCommandLists(1, lists);
    target->Release();
    hr = presenter.swap_chain->Present(kPresentSyncInterval, 0);
    queue->pc->Signal(presenter.fence, ++presenter.presented);
    presenter.done_at[index] = presenter.presented;
    return hr;
}

}  // namespace

HRESULT STDMETHODCALLTYPE queue_present_x(ConsoleQueue* self, UINT plane_count, const ConsolePresentPlane* planes,
                                          const void* parameters) {
    if (plane_count == 0 || !planes || planes[0].resource_count == 0 || !planes[0].resources ||
        !planes[0].resources[0])
        return E_INVALIDARG;
    static std::atomic<int> logged{0};
    if (plane_count > 1 && logged.load(std::memory_order_relaxed) < kLoggedPresents)
        G6SHIM_WARN("PresentX: %u planes, only the first is shown", plane_count);
    Presenters& p = presenters();
    std::lock_guard<std::mutex> lock(p.mutex);
    const HRESULT hr = present(self, planes[0].resources[0], p.by_queue[self->pc]);
    if (FAILED(hr) || logged.fetch_add(1, std::memory_order_relaxed) < kLoggedPresents)
        G6SHIM_INFO("PresentX(%u planes, token 0x%llX, parameters %p) -> 0x%08lX", plane_count,
                    static_cast<unsigned long long>(planes[0].token), parameters, hr);
    return hr;
}

}  // namespace g6shim::d3d12
