// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api.hpp"
#include "translate.hpp"

#include "../d3d12_xs/private.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace g6shim::upscaler {

namespace {

constexpr wchar_t kHostLoader[] = L"amd_fidelityfx_loader_dx12.dll";

struct HostApi {
    HMODULE module = nullptr;
    CreateContextFn create = nullptr;
    DestroyContextFn destroy = nullptr;
    ConfigureFn configure = nullptr;
    QueryFn query = nullptr;
    DispatchFn dispatch = nullptr;
};

std::wstring own_folder() {
    HMODULE self = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&own_folder), &self) == 0)
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring folder(path, length);
    const std::size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash + 1);
}

const HostApi* host_api() {
    static const HostApi api = [] {
        HostApi loaded;
        const std::wstring path = own_folder() + kHostLoader;
        loaded.module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (loaded.module == nullptr) {
            G6SHIM_ERROR("upscaler: the host upscaler loader was not found next to the game (error %lu)",
                         ::GetLastError());
            return loaded;
        }
        loaded.create = reinterpret_cast<CreateContextFn>(::GetProcAddress(loaded.module, "ffxCreateContext"));
        loaded.destroy = reinterpret_cast<DestroyContextFn>(::GetProcAddress(loaded.module, "ffxDestroyContext"));
        loaded.configure = reinterpret_cast<ConfigureFn>(::GetProcAddress(loaded.module, "ffxConfigure"));
        loaded.query = reinterpret_cast<QueryFn>(::GetProcAddress(loaded.module, "ffxQuery"));
        loaded.dispatch = reinterpret_cast<DispatchFn>(::GetProcAddress(loaded.module, "ffxDispatch"));
        if (!loaded.create || !loaded.destroy || !loaded.configure || !loaded.query || !loaded.dispatch) {
            G6SHIM_ERROR("upscaler: the host upscaler loader lacks an entry point");
            loaded.create = nullptr;
        } else {
            G6SHIM_INFO("upscaler: host upscaler loader ready");
        }
        return loaded;
    }();
    return api.create != nullptr ? &api : nullptr;
}

const d3d12_xs::private_api::UpscalerBridge* bridge() {
    static const d3d12_xs::private_api::UpscalerBridge* found = d3d12_xs::private_api::resolve_upscaler_bridge();
    return found;
}

std::mutex g_mutex;
std::map<Context, std::unique_ptr<CreateChain>> g_chains;
std::set<StructType> g_reported;

void report_once(const char* what, StructType type) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_reported.insert(type).second) return;
    }
    G6SHIM_WARN("upscaler: %s 0x%llx is not translated", what, static_cast<unsigned long long>(type));
}

enum class Need { kOptional, kRequired };

bool map_resource(const d3d12_xs::private_api::UpscalerBridge& host, void* title_list, Resource& resource,
                  std::uint32_t wanted, Need need, const char* name) {
    if (resource.resource == nullptr) {
        if (need == Need::kOptional) return true;
        G6SHIM_WARN("upscaler: the title passed no %s resource", name);
        return false;
    }
    std::uint32_t state = 0;
    ID3D12Resource* hosted = host.resource(title_list, resource.resource, wanted, &state);
    const std::optional<std::uint32_t> ffx_state = resource_state(state);
    if (hosted == nullptr || !ffx_state) {
        G6SHIM_WARN("upscaler: the %s resource has no host texture in state 0x%x (got 0x%x)", name, wanted, state);
        if (need == Need::kOptional) {
            resource = Resource{};
            return true;
        }
        return false;
    }
    resource.description = describe(hosted->GetDesc(), resource.description);
    resource.resource = hosted;
    resource.state = *ffx_state;
    return true;
}

ReturnCode dispatch_upscale(Context* context, const DispatchUpscale& title) {
    const auto* host = bridge();
    const HostApi* api = host_api();
    if (host == nullptr || api == nullptr) return kNoProvider;
    DispatchUpscale work = title;
    for (const StructType type : chained_types(title.header.next)) report_once("an upscale dispatch extension", type);
    work.header.next = nullptr;
    work.command_list = host->command_list(title.command_list);
    if (work.command_list == nullptr) return kParameter;
    constexpr std::uint32_t kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr std::uint32_t kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const bool mapped = map_resource(*host, title.command_list, work.color, kRead, Need::kRequired, "color") &&
                        map_resource(*host, title.command_list, work.depth, kRead, Need::kRequired, "depth") &&
                        map_resource(*host, title.command_list, work.motion_vectors, kRead, Need::kRequired,
                                     "motion vector") &&
                        map_resource(*host, title.command_list, work.exposure, kRead, Need::kOptional, "exposure") &&
                        map_resource(*host, title.command_list, work.reactive, kRead, Need::kOptional, "reactive") &&
                        map_resource(*host, title.command_list, work.transparency, kRead, Need::kOptional,
                                     "transparency") &&
                        map_resource(*host, title.command_list, work.output, kWrite, Need::kRequired, "output");
    if (!mapped) return kParameter;
    const ReturnCode result = api->dispatch(context, &work.header);
    host->forget_bindings(title.command_list);
    if (result != kOk) G6SHIM_WARN("upscaler: the host upscale dispatch failed with %u", result);
    return result;
}

ReturnCode dispatch_reactive_mask(Context* context, const DispatchReactiveMask& title) {
    const auto* host = bridge();
    const HostApi* api = host_api();
    if (host == nullptr || api == nullptr) return kNoProvider;
    DispatchReactiveMask work = title;
    work.header.next = nullptr;
    work.command_list = host->command_list(title.command_list);
    if (work.command_list == nullptr) return kParameter;
    constexpr std::uint32_t kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr std::uint32_t kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const bool mapped =
        map_resource(*host, title.command_list, work.opaque_color, kRead, Need::kRequired, "opaque color") &&
        map_resource(*host, title.command_list, work.color, kRead, Need::kRequired, "color") &&
        map_resource(*host, title.command_list, work.out_reactive, kWrite, Need::kRequired, "reactive output");
    if (!mapped) return kParameter;
    const ReturnCode result = api->dispatch(context, &work.header);
    host->forget_bindings(title.command_list);
    return result;
}

}  // namespace

}  // namespace g6shim::upscaler

using namespace g6shim::upscaler;

extern "C" ReturnCode g6shim_ffxCreateContext(Context* context, Header* desc, const AllocationCallbacks* memory) {
    const HostApi* api = host_api();
    const auto* host = bridge();
    if (api == nullptr || host == nullptr || context == nullptr || desc == nullptr) return kNoProvider;
    auto chain = std::make_unique<CreateChain>(translate_create(desc, host->device()));
    for (const StructType type : chain->dropped) report_once("a create descriptor", type);
    if (!chain->has_upscale) {
        report_once("an effect", desc->type);
        return kUnknownType;
    }
    ReturnCode result = api->create(context, chain->head(true), memory);
    if (result != kOk && chain->has_version) {
        G6SHIM_WARN("upscaler: creating the upscaler at version 0x%x failed with %u; trying the newest",
                    chain->version.version, result);
        result = api->create(context, chain->head(false), memory);
    }
    if (result != kOk) {
        G6SHIM_ERROR("upscaler: the host upscaler was not created (%u)", result);
        return result;
    }
    QueryProviderVersion provider{};
    provider.header.type = kQueryProviderVersion;
    if (api->query(context, &provider.header) == kOk && provider.name != nullptr)
        G6SHIM_INFO("upscaler: %ux%u to %ux%u through the host provider %s", chain->upscale.max_render.width,
                    chain->upscale.max_render.height, chain->upscale.max_upscale.width,
                    chain->upscale.max_upscale.height, provider.name);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_chains[*context] = std::move(chain);
    return kOk;
}

extern "C" ReturnCode g6shim_ffxDestroyContext(Context* context, const AllocationCallbacks* memory) {
    const HostApi* api = host_api();
    if (api == nullptr || context == nullptr) return kNoProvider;
    const Context key = *context;
    const ReturnCode result = api->destroy(context, memory);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_chains.erase(key);
    return result;
}

extern "C" ReturnCode g6shim_ffxConfigure(Context* context, const Header* desc) {
    const HostApi* api = host_api();
    if (api == nullptr) return kNoProvider;
    return api->configure(context, desc);
}

extern "C" ReturnCode g6shim_ffxQuery(Context* context, Header* desc) {
    const HostApi* api = host_api();
    const auto* host = bridge();
    if (api == nullptr || desc == nullptr) return kNoProvider;
    if (desc->type == kQueryVersions && host != nullptr) {
        QueryVersions work = *reinterpret_cast<QueryVersions*>(desc);
        work.device = host->device();
        return api->query(context, &work.header);
    }
    if (desc->type == kQueryMemoryUsage && host != nullptr) {
        QueryMemoryUsage work = *reinterpret_cast<QueryMemoryUsage*>(desc);
        work.device = host->device();
        return api->query(context, &work.header);
    }
    return api->query(context, desc);
}

extern "C" ReturnCode g6shim_ffxDispatch(Context* context, const Header* desc) {
    if (desc == nullptr) return kParameter;
    if (desc->type == kDispatchUpscale)
        return dispatch_upscale(context, *reinterpret_cast<const DispatchUpscale*>(desc));
    if (desc->type == kDispatchReactiveMask)
        return dispatch_reactive_mask(context, *reinterpret_cast<const DispatchReactiveMask*>(desc));
    report_once("a dispatch", desc->type);
    return kUnknownType;
}
