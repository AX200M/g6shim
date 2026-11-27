// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.hpp"

#include "title.hpp"
#include "xasync.hpp"

#include <g6shim/drives.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <system_error>

namespace g6shim::xgameruntime::storage {

namespace {

using This = Object*;

const std::uint8_t kPrompt = 0;

std::filesystem::path storage_folder() {
    const Title value = title();
    if (!value.storage.empty()) return value.storage;
    return persistent_storage_folder();
}

SpaceInfo space_of(const std::filesystem::path& folder, std::uint64_t quota) {
    const std::uint64_t used = used_bytes(folder);
    const std::uint64_t free = quota > used ? quota - used : 0;
    return SpaceInfo{std::min(free, disk_free(folder)), free, used, quota};
}

HResult path_size(This, std::size_t* out) {
    const std::optional<std::string> path = drive_path();
    if (!path) return kNotSupported;
    return put(out, path->size() + 1);
}

HResult path(This, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    const std::optional<std::string> path = drive_path();
    if (!path) return kNotSupported;
    if (size <= path->size()) return kFail;
    return put_string(*path, size, buffer, used);
}

HResult space_info(This, SpaceInfo* out) { return put(out, space()); }

HResult prompt_for_space(This, std::uint64_t bytes, xasync::AsyncBlock* block) {
    const HResult result = bytes <= space().total_free ? kOk : kAbort;
    return xasync::finished(block, &kPrompt, result);
}

HResult prompt_result(This, xasync::AsyncBlock* block) {
    return xasync::XAsyncGetResult(block, &kPrompt, 0, nullptr, nullptr);
}

const std::array<const void*, 8> kVtable = {
    slot(&missing<0>),   slot(&add_ref),  slot(&add_ref),       slot(&path_size),
    slot(&path),         slot(&space_info), slot(&prompt_for_space), slot(&prompt_result)};

}  // namespace

const Object kObject{kVtable.data(), "persistent local storage"};

std::optional<std::string> drive_path() {
    const std::filesystem::path folder = storage_folder();
    if (folder.empty()) return std::nullopt;
    const int letter = g6shim::drives::mount_drive(folder.c_str());
    if (letter == 0) return std::nullopt;
    return std::string{static_cast<char>(letter), ':', '\\'};
}

std::uint64_t used_bytes(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) return 0;
    std::uint64_t total = 0;
    for (const std::filesystem::directory_entry& entry : entries) {
        std::error_code kind_error;
        if (entry.is_directory(kind_error)) {
            total += used_bytes(entry.path());
        } else if (!kind_error) {
            std::error_code size_error;
            const std::uintmax_t size = entry.file_size(size_error);
            if (!size_error) total += size;
        }
    }
    return total;
}

std::uint64_t disk_free(const std::filesystem::path& folder) {
    std::uint64_t free = 0;
    if (GetDiskFreeSpaceExW(folder.c_str(), reinterpret_cast<PULARGE_INTEGER>(&free), nullptr, nullptr) != 0)
        return free;
    return UINT64_MAX;
}

SpaceInfo space() {
    const Title value = title();
    return space_of(storage_folder(), value.storage_bytes);
}

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kIid, &kObject}, {kProvider, kIid2026, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::storage

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::storage_interfaces() {
    return storage::interfaces();
}
