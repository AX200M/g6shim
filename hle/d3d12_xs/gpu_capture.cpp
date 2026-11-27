// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu_capture.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

namespace g6shim::d3d12_xs::gpu_capture {

namespace {

constexpr char kToolVariable[] = "G6SHIM_GPU_CAPTURE";
constexpr wchar_t kTriggerName[] = L"g6shim_capture.trigger";
constexpr wchar_t kCaptureFolder[] = L"g6shim_captures";
constexpr char kCapturePrefix[] = "g6shim";

using GetApi = int(__cdecl*)(int version, void** api);

Api* g_api = nullptr;
std::filesystem::path g_trigger;
std::atomic<std::uint32_t> g_reported{0};

std::optional<std::filesystem::path> executable_folder() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::nullopt;
    return std::filesystem::path(std::wstring(path, length)).parent_path();
}

void report_new_captures() {
    const std::uint32_t count = g_api->capture_count();
    std::uint32_t seen = g_reported.load(std::memory_order_relaxed);
    while (seen < count) {
        char path[MAX_PATH] = {};
        std::uint32_t length = MAX_PATH;
        std::uint64_t time = 0;
        if (g_api->get_capture(seen, path, &length, &time) != 0)
            G6SHIM_INFO("d3d12_xs: GPU capture %u saved to %s", seen + 1, path);
        ++seen;
    }
    g_reported.store(seen, std::memory_order_relaxed);
}

}  // namespace

void load() {
    const std::optional<std::wstring> tool = settings::get_wide(kToolVariable);
    if (!tool || tool->empty()) return;
    const std::optional<std::filesystem::path> folder = executable_folder();
    if (!folder) return;
    HMODULE module = ::LoadLibraryW(tool->c_str());
    if (module == nullptr) {
        G6SHIM_WARN("d3d12_xs: the GPU capture tool did not load (error %lu)", ::GetLastError());
        return;
    }
    const auto get_api = reinterpret_cast<GetApi>(::GetProcAddress(module, "RENDERDOC_GetAPI"));
    void* table = nullptr;
    if (get_api == nullptr || get_api(kApiVersion, &table) != 1 || table == nullptr) {
        G6SHIM_WARN("d3d12_xs: the GPU capture tool offers no in-application interface");
        return;
    }
    g_api = static_cast<Api*>(table);
    int major = 0;
    int minor = 0;
    int patch = 0;
    g_api->get_version(&major, &minor, &patch);
    g_api->set_option_u32(kOptionSoftMemoryLimit, kSoftMemoryLimitMegabytes);
    g_api->set_capture_keys(nullptr, 0);
    g_api->set_focus_keys(nullptr, 0);
    g_api->mask_overlay_bits(0, 0);
    std::error_code error;
    const std::filesystem::path captures = *folder / kCaptureFolder;
    std::filesystem::create_directories(captures, error);
    const std::string prefix = (captures / kCapturePrefix).string();
    g_api->set_path_template(prefix.c_str());
    g_trigger = *folder / kTriggerName;
    G6SHIM_INFO("d3d12_xs: GPU capture tool %d.%d.%d ready; create %s to capture the next frame", major, minor,
                patch, g_trigger.string().c_str());
}

void frame_presented() {
    if (g_api == nullptr) return;
    report_new_captures();
    std::error_code error;
    if (!std::filesystem::exists(g_trigger, error)) return;
    std::filesystem::remove(g_trigger, error);
    g_api->trigger();
    G6SHIM_INFO("d3d12_xs: GPU capture requested for the next frame");
}

}  // namespace g6shim::d3d12_xs::gpu_capture
