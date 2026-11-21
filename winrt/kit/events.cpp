// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>

#include <windows.h>
#include <unknwn.h>

#include <g6shim/log.hpp>

#include <array>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace g6shim::winrt {

namespace {

struct Registration {
    void* object;
    std::size_t method;
    IUnknown* handler;
};

constexpr std::size_t kMaxEventMethods = kMaxSlots - kInspectableSlots;

std::mutex g_mutex;
std::map<std::int64_t, Registration> g_handlers;
std::int64_t g_next_token = kFirstEventToken;

template <std::size_t N>
HRESULT __stdcall AddAt(void* self, IUnknown* handler, std::int64_t* token) {
    return add_event_handler(self, N, handler, token);
}

HRESULT __stdcall Remove(void*, std::int64_t token) { return remove_event_handler(token); }

template <std::size_t... I>
std::array<const void*, kMaxEventMethods> add_table(std::index_sequence<I...>) {
    return {fn(&AddAt<I>)...};
}

}  // namespace

long add_event_handler(void* self, std::size_t method, void* handler, std::int64_t* token) {
    if (!handler || !token) return E_INVALIDARG;
    auto* h = static_cast<IUnknown*>(handler);
    h->AddRef();
    std::lock_guard<std::mutex> lock(g_mutex);
    *token = g_next_token++;
    g_handlers[*token] = {self, method, h};
    return S_OK;
}

long remove_event_handler(std::int64_t token) {
    IUnknown* handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_handlers.find(token);
        if (it == g_handlers.end()) return S_OK;
        handler = it->second.handler;
        g_handlers.erase(it);
    }
    handler->Release();
    return S_OK;
}

bool is_event_token(std::int64_t token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_handlers.count(token) != 0;
}

const void* event_add(std::size_t method) {
    static const auto table = add_table(std::make_index_sequence<kMaxEventMethods>{});
    return method < kMaxEventMethods ? table[method] : nullptr;
}

const void* event_remove() { return fn(&Remove); }

std::size_t raise_event(void* object, std::size_t method, void* sender, void* args) {
    std::vector<IUnknown*> handlers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [token, r] : g_handlers)
            if (r.object == object && r.method == method) {
                r.handler->AddRef();
                handlers.push_back(r.handler);
            }
    }
    for (IUnknown* h : handlers) {
        const HRESULT hr = vcall(h, kDelegateInvokeSlot, sender, args);
        if (FAILED(hr)) G6SHIM_WARN("event handler (method %zu) returned 0x%08lX", method, static_cast<unsigned long>(hr));
        h->Release();
    }
    return handlers.size();
}

}  // namespace g6shim::winrt
