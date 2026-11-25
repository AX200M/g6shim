// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::d3d12_xs::pipeline_library {

inline constexpr wchar_t kLibraryFile[] = L"host-pipelines.bin";
inline constexpr wchar_t kEarlyFile[] = L"early-pipelines.txt";
inline constexpr std::size_t kEarlyMax = 8192;

inline std::atomic<std::uint64_t> made{0};
inline std::atomic<std::uint64_t> loaded{0};
inline std::atomic<std::uint64_t> early{0};

struct Library {
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> library;
    std::vector<std::uint8_t> blob;
    std::uint64_t added = 0;

    std::size_t serialized_size() const;
};

std::optional<Library> open(ID3D12Device1* device, std::vector<std::uint8_t> blob);
std::optional<std::filesystem::path> directory();
void use_directory(const std::filesystem::path& dir);

std::string name(std::string_view kind, std::span<const std::uint8_t> root,
                 std::span<const std::span<const std::uint8_t>> code, std::string_view state);
std::string name(std::string_view kind, std::span<const std::uint8_t> root,
                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
std::string name(std::string_view kind, std::span<const std::uint8_t> root,
                 const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc);
std::string name(std::string_view kind, std::span<const std::uint8_t> root,
                 const D3D12_PIPELINE_STATE_STREAM_DESC& desc);

inline constexpr std::size_t kStreamAlign = 8;

template <typename T>
constexpr std::size_t stream_subobject_value() {
    return (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + alignof(T) - 1) & ~(alignof(T) - 1);
}

template <typename T>
constexpr std::size_t stream_subobject_bytes() {
    return (stream_subobject_value<T>() + sizeof(T) + kStreamAlign - 1) & ~(kStreamAlign - 1);
}


using Load = std::function<HRESULT(ID3D12PipelineLibrary1*, const wchar_t*)>;
using Create = std::function<HRESULT(ID3D12PipelineState**)>;

HRESULT pipeline(const std::string& name, const Load& load, const Create& create, ID3D12PipelineState** out);

void save(bool now);
bool wanted_early(const std::string& key);
void note_used(const std::string& key);

}  // namespace g6shim::d3d12_xs::pipeline_library
