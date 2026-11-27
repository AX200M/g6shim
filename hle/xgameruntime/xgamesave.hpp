// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace g6shim::xgameruntime::xgamesave {

inline constexpr Guid kProvider = guid(0x704c3f58, 0xe629, 0x4cc2, 0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f,
                                       0xe2);
inline constexpr Guid kIid2026 = guid(0x1bfff3af, 0xf14a, 0x40a3, 0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93,
                                      0xf9);

inline constexpr HResult kInvalidContainerName = static_cast<HResult>(0x80830001u);
inline constexpr HResult kNoAccess = static_cast<HResult>(0x80830002u);
inline constexpr HResult kOutOfLocalStorage = static_cast<HResult>(0x80830003u);
inline constexpr HResult kUpdateTooBig = static_cast<HResult>(0x80830005u);
inline constexpr HResult kQuotaExceeded = static_cast<HResult>(0x80830006u);
inline constexpr HResult kBufferTooSmall = static_cast<HResult>(0x80830007u);
inline constexpr HResult kBlobNotFound = static_cast<HResult>(0x80830008u);

inline constexpr std::uint64_t kQuota = std::uint64_t{256} << 20;
inline constexpr std::size_t kMaxUpdate = std::size_t{16} << 20;
inline constexpr std::size_t kBlobEntry = 24;
inline constexpr std::size_t kReadableName = 100;
inline constexpr std::size_t kMaxManifestBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxContainerName = 256;
inline constexpr char kManifestFile[] = "manifest";

struct ContainerInfo {
    const char* name;
    const char* display_name;
    std::uint32_t blob_count;
    std::uint64_t total_size;
    std::int64_t last_modified;
    bool needs_sync;
};

struct BlobInfo {
    const char* name;
    std::uint32_t size;
};

static_assert(sizeof(ContainerInfo) == 48);
static_assert(sizeof(BlobInfo) == 16);

struct Version {
    std::filesystem::path dir;
    std::uint64_t number = 0;
    std::string name;
    std::string display;
    std::vector<std::string> blobs;

    std::filesystem::path blob(std::size_t index) const;
    std::vector<std::pair<std::string, std::uint64_t>> sizes() const;
};

std::string file_name(std::string_view name);
std::optional<std::string> decode_name(std::string_view file);
std::filesystem::path container_dir(const std::filesystem::path& root, std::string_view name);
bool valid_container(std::string_view name);

std::vector<std::uint8_t> manifest_bytes(std::string_view name, std::string_view display,
                                         const std::vector<std::string>& blobs);
std::optional<std::vector<std::string>> parse_manifest(std::span<const std::uint8_t> bytes);

std::optional<Version> current(const std::filesystem::path& dir, std::error_code& error);
std::optional<Version> current_named(const std::filesystem::path& dir, std::string_view name,
                                     std::error_code& error);
std::vector<std::pair<std::string, std::filesystem::path>> containers(const std::filesystem::path& root);

struct Provider {
    std::filesystem::path root;
};

struct Container {
    std::string name;
    std::filesystem::path dir;
};

struct Update {
    std::string name;
    std::filesystem::path dir;
    std::optional<std::string> display;
    std::vector<std::pair<std::string, std::optional<std::vector<std::uint8_t>>>> changes;
};

std::optional<Provider> open_provider(const std::filesystem::path& saves, std::optional<std::string> config,
                                      HResult& error);
HResult submit(Update& update);
HResult delete_container(const std::filesystem::path& root, std::string_view name);
HResult read_container(const Container& container, const std::optional<std::vector<std::string>>& names,
                       std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& blobs);

std::optional<HResult> no_room(std::uint64_t written, std::uint64_t replaced, std::uint64_t used,
                               std::uint64_t free);
std::uint64_t save_used_bytes(const std::filesystem::path& root);
std::uint64_t remaining_bytes(const std::filesystem::path& root);

using CrashHook = void (*)();
CrashHook& crash_hook();

extern const Object kObject;

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xgamesave
