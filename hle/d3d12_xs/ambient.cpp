// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ambient.hpp"

#include "g_ambient_air.h"
#include "g_ambient_gather.h"
#include "g_ambient_keep.h"
#include "g_ambient_mean.h"
#include "g_ambient_place.h"
#include "g_ambient_raise.h"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>

namespace g6shim::d3d12_xs::ambient {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::string_view kConstantPrefix = "constant:";
constexpr std::uint32_t kCellsX = 64;
constexpr std::uint32_t kCellsY = 36;
constexpr std::uint32_t kCells = kCellsX * kCellsY;
constexpr std::uint64_t kFloat4Bytes = 16;
constexpr std::uint64_t kCellBytes = (2 * kCells + 1) * kFloat4Bytes;
constexpr std::uint64_t kPointBytes = 160 * 90 * 3 * kFloat4Bytes;
constexpr std::uint64_t kStatsBytes = 256;
constexpr std::uint32_t kKeepGroups[2] = {kCellsX / 8, kCellsY / 4};
constexpr std::uint32_t kGatherGroups[2] = {160 / 8, 90 / 2};
constexpr std::uint32_t kRaiseTile = 8;
constexpr std::uint32_t kAirTile[3] = {8, 8, 2};
constexpr std::uint32_t kConstants = 15;
constexpr float kFrameFollow = 0.04f;
constexpr float kMostOfLeftover = 4.0f;
constexpr float kDefaultFrameMs = 33.0f;
constexpr double kPauseMs = 1000.0;
constexpr std::uint32_t kAnnounced = 4;
constexpr DXGI_FORMAT kLightFormat = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT kRadianceFormat = DXGI_FORMAT_R11G11B10_FLOAT;

std::optional<float> positive(std::string_view text) {
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value) || value <= 0)
        return std::nullopt;
    return value;
}

std::optional<float> not_negative(std::string_view text) {
    if (text == "0") return 0.0f;
    return positive(text);
}

std::optional<std::uint32_t> number(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<Place> place_at(const std::vector<std::string>& words, std::size_t at) {
    if (words.size() < at + 2) return std::nullopt;
    const auto slot = number(words[at]);
    const auto offset = number(words[at + 1]);
    if (!slot || !offset || *slot > 0xFF) return std::nullopt;
    return Place{static_cast<std::uint8_t>(*slot), *offset};
}

std::string read_text(const std::wstring& path) {
    std::ifstream in{std::filesystem::path(path)};
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Passes {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> keep;
    ComPtr<ID3D12PipelineState> mean;
    ComPtr<ID3D12PipelineState> place;
    ComPtr<ID3D12PipelineState> gather;
    ComPtr<ID3D12PipelineState> raise;
    ComPtr<ID3D12PipelineState> air;
    ComPtr<ID3D12Resource> cells;
    ComPtr<ID3D12Resource> points;
    ComPtr<ID3D12Resource> stats;
};

ComPtr<ID3D12Resource> unordered_buffer(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&buffer))))
        return nullptr;
    host::name(buffer.Get(), "ambient stand-in");
    return buffer;
}

ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                     std::span<const unsigned char> code) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {code.data(), code.size()};
    ComPtr<ID3D12PipelineState> made;
    if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&made)))) return nullptr;
    return made;
}

std::optional<Passes> make_passes() {
    host::Host* gpu = host::host();
    if (gpu == nullptr || gpu->device() == nullptr) return std::nullopt;
    ID3D12Device* device = gpu->device();
    Passes made;
    if (FAILED(device->CreateRootSignature(0, g_ambient_keep, sizeof(g_ambient_keep), IID_PPV_ARGS(&made.root))))
        return std::nullopt;
    made.keep = pipeline(device, made.root.Get(), g_ambient_keep);
    made.mean = pipeline(device, made.root.Get(), g_ambient_mean);
    made.place = pipeline(device, made.root.Get(), g_ambient_place);
    made.gather = pipeline(device, made.root.Get(), g_ambient_gather);
    made.raise = pipeline(device, made.root.Get(), g_ambient_raise);
    made.air = pipeline(device, made.root.Get(), g_ambient_air);
    made.cells = unordered_buffer(device, kCellBytes);
    made.points = unordered_buffer(device, kPointBytes);
    made.stats = unordered_buffer(device, kStatsBytes);
    if (!made.keep || !made.mean || !made.place || !made.gather || !made.raise || !made.air || !made.cells ||
        !made.points || !made.stats)
        return std::nullopt;
    return made;
}

const Passes* passes() {
    static std::once_flag once;
    static std::optional<Passes> made;
    std::call_once(once, [] {
        made = make_passes();
        if (!made) G6SHIM_WARN("d3d12_xs: no stand-in for the indirect light: its passes were not made");
    });
    return made ? &*made : nullptr;
}

std::atomic<bool> g_kept{false};
std::atomic<std::uint32_t> g_announced{0};
std::mutex g_clock_mutex;
std::chrono::steady_clock::time_point g_last_keep{};
float g_frame_ms = kDefaultFrameMs;

float keep_frame_ms() {
    std::lock_guard<std::mutex> lock(g_clock_mutex);
    const auto now = std::chrono::steady_clock::now();
    const bool first = g_last_keep == std::chrono::steady_clock::time_point{};
    const double ms = std::chrono::duration<double, std::milli>(now - g_last_keep).count();
    g_last_keep = now;
    g_frame_ms = first || ms > kPauseMs ? kDefaultFrameMs : static_cast<float>(std::max(ms, 1.0));
    return g_frame_ms;
}

float last_frame_ms() {
    std::lock_guard<std::mutex> lock(g_clock_mutex);
    return g_frame_ms;
}

void uav_barrier(ID3D12GraphicsCommandList* commands) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    commands->ResourceBarrier(1, &barrier);
}

const char* role_name(Role role) {
    switch (role) {
    case Role::kResolve: return "resolve";
    case Role::kRadiance: return "radiance";
    case Role::kAir: return "air";
    }
    return "?";
}

}  // namespace

Settings parse_settings(std::string_view level, std::string_view roles_text) {
    Settings out;
    if (level.starts_with(kConstantPrefix)) {
        if (const auto value = positive(level.substr(kConstantPrefix.size()))) {
            out.mode = Mode::kConstant;
            out.level = *value;
        }
    } else if (const auto value = positive(level)) {
        out.mode = Mode::kEstimate;
        out.level = *value;
    }
    std::istringstream lines{std::string(roles_text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream split(line);
        std::vector<std::string> words{std::istream_iterator<std::string>(split), std::istream_iterator<std::string>()};
        if (words.empty() || words[0].starts_with("#")) continue;
        const std::string& key = words[0];
        if ((key == "resolve" || key == "radiance" || key == "air") && words.size() >= 4) {
            RoleSetting role;
            role.role = key == "resolve" ? Role::kResolve : key == "radiance" ? Role::kRadiance : Role::kAir;
            role.name = words[1];
            const auto image = place_at(words, 2);
            if (!image) continue;
            role.image = *image;
            if (role.role == Role::kResolve) {
                const auto depth = place_at(words, 4);
                if (!depth) continue;
                role.depth = *depth;
            }
            out.roles.push_back(std::move(role));
        } else if (key == "camera" && words.size() >= 3) {
            if (const auto near_plane = positive(words[1])) out.near_plane = *near_plane;
            if (const auto tan_half_y = positive(words[2])) out.tan_half_y = *tan_half_y;
        } else if (key == "shade" && words.size() >= 3) {
            if (const auto strength = not_negative(words[1])) out.shade[0] = *strength;
            if (const auto radius = positive(words[2])) out.shade[1] = *radius;
        } else if (key == "settle" && words.size() >= 2) {
            if (const auto ms = not_negative(words[1])) out.settle_ms = *ms;
            if (words.size() >= 3)
                if (const auto jump = not_negative(words[2])) out.jump = *jump;
            if (words.size() >= 4)
                if (const auto fresh = not_negative(words[3])) out.fresh = *fresh;
        } else if (key == "air-cap" && words.size() >= 2) {
            if (const auto cap = not_negative(words[1])) out.air_cap = *cap;
        }
    }
    return out;
}

const Settings& settings() {
    static const Settings made = [] {
        const std::string level = g6shim::settings::get("G6SHIM_AMBIENT").value_or("");
        const std::optional<std::wstring> path = g6shim::settings::get_wide("G6SHIM_AMBIENT_ROLES");
        Settings parsed = parse_settings(level, path ? read_text(*path) : std::string());
        if (parsed.mode != Mode::kOff)
            G6SHIM_INFO("d3d12_xs: indirect light stand-in %s at %.2f with %zu roles",
                        parsed.mode == Mode::kConstant ? "constant" : "estimate", parsed.level, parsed.roles.size());
        return parsed;
    }();
    return made;
}

bool on() { return settings().mode != Mode::kOff && !settings().roles.empty(); }

std::optional<Role> role_of(std::span<const std::uint8_t> container) {
    if (!on()) return std::nullopt;
    for (const RoleSetting& role : settings().roles) {
        if (role.name.empty()) continue;
        const auto found = std::search(container.begin(), container.end(), role.name.begin(), role.name.end(),
                                       [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); });
        if (found != container.end()) return role.role;
    }
    return std::nullopt;
}

const RoleSetting* setting(Role role) {
    for (const RoleSetting& entry : settings().roles)
        if (entry.role == role) return &entry;
    return nullptr;
}

bool image_fits(Role role, const D3D12_RESOURCE_DESC& image, const D3D12_RESOURCE_DESC* depth) {
    const bool volume = image.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (volume != (role == Role::kAir)) return false;
    if (role == Role::kAir) return true;
    if (image.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (image.Format != (role == Role::kResolve ? kLightFormat : kRadianceFormat)) return false;
    if (role != Role::kResolve) return true;
    return depth != nullptr && depth->Width == image.Width && depth->Height == image.Height;
}

bool passes_ready() { return passes() != nullptr; }

bool after(host::HostList& list, Role role, const Views& views) { return record(list, role, views, settings()); }

bool record(host::HostList& list, Role role, const Views& views, const Settings& set) {
    if (set.mode == Mode::kOff) return false;
    const Passes* made = passes();
    if (made == nullptr) return false;
    const bool constant = set.mode == Mode::kConstant;
    if (role != Role::kRadiance && !constant && !g_kept.load(std::memory_order_relaxed)) return false;
    if (role == Role::kAir && (constant || set.air_cap == 0.0f)) return false;
    if (role == Role::kRadiance && constant) return false;
    if (g_announced.fetch_add(1, std::memory_order_relaxed) < kAnnounced)
        G6SHIM_INFO("d3d12_xs: indirect light stand-in after a %s pass (%ux%u)", role_name(role), views.width,
                    views.height);
    const float frame_ms = role == Role::kRadiance ? keep_frame_ms() : last_frame_ms();
    const float follow = role == Role::kRadiance && set.settle_ms > 0.0f
                             ? 1.0f - std::exp(-frame_ms / set.settle_ms)
                             : kFrameFollow;
    std::array<std::uint32_t, kConstants> words{};
    words[0] = std::bit_cast<std::uint32_t>(constant ? 0.0f : set.level);
    words[1] = std::bit_cast<std::uint32_t>(constant ? set.level : 0.0f);
    words[2] = std::bit_cast<std::uint32_t>(set.shade[0]);
    words[3] = std::bit_cast<std::uint32_t>(set.shade[1]);
    words[4] = std::bit_cast<std::uint32_t>(set.near_plane);
    words[5] = std::bit_cast<std::uint32_t>(set.tan_half_y);
    words[6] = views.width;
    words[7] = views.height;
    words[8] = std::bit_cast<std::uint32_t>(follow);
    words[9] = std::bit_cast<std::uint32_t>(kMostOfLeftover);
    words[10] = std::bit_cast<std::uint32_t>(set.air_cap);
    words[11] = 0;
    words[12] = std::bit_cast<std::uint32_t>(set.settle_ms > 0.0f ? set.jump : 0.0f);
    words[13] = std::bit_cast<std::uint32_t>(set.settle_ms > 0.0f ? set.fresh : 0.0f);
    words[14] = std::bit_cast<std::uint32_t>(frame_ms / 1000.0f);
    list.keep(made->cells.Get());
    list.keep(made->points.Get());
    list.keep(made->stats.Get());
    list.keep(made->root.Get());
    ID3D12GraphicsCommandList* commands = list.list();
    uav_barrier(commands);
    commands->SetComputeRootSignature(made->root.Get());
    commands->SetComputeRoot32BitConstants(0, kConstants, words.data(), 0);
    commands->SetComputeRootDescriptorTable(1, views.depth);
    commands->SetComputeRootDescriptorTable(2, views.image);
    commands->SetComputeRootUnorderedAccessView(3, made->cells->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(4, made->points->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(5, made->stats->GetGPUVirtualAddress());
    const auto run = [&](ID3D12PipelineState* pso, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        list.keep(pso);
        commands->SetPipelineState(pso);
        commands->Dispatch(x, y, z);
        uav_barrier(commands);
    };
    const auto tiles = [](std::uint32_t size, std::uint32_t tile) { return (size + tile - 1) / tile; };
    switch (role) {
    case Role::kRadiance:
        run(made->keep.Get(), kKeepGroups[0], kKeepGroups[1], 1);
        run(made->mean.Get(), 1, 1, 1);
        g_kept.store(true, std::memory_order_relaxed);
        break;
    case Role::kAir:
        run(made->air.Get(), tiles(views.width, kAirTile[0]), tiles(views.height, kAirTile[1]),
            tiles(views.slices, kAirTile[2]));
        break;
    case Role::kResolve:
        if (!constant) {
            run(made->place.Get(), kKeepGroups[0], kKeepGroups[1], 1);
            run(made->gather.Get(), kGatherGroups[0], kGatherGroups[1], 1);
        }
        run(made->raise.Get(), tiles(views.width, kRaiseTile), tiles(views.height, kRaiseTile), 1);
        break;
    }
    return true;
}

}  // namespace g6shim::d3d12_xs::ambient
