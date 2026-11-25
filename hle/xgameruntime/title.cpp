// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "title.hpp"

#include "xgame.hpp"

#include <g6shim/drives.hpp>
#include <g6shim/settings.hpp>

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <optional>

namespace g6shim::xgameruntime {

namespace {

constexpr char kStoreIdElement[] = "Game/StoreId";
constexpr char kStorageSizeElement[] = "Game/PersistentLocalStorage/SizeMB";
constexpr char kDataVariable[] = "G6SHIM_DATA_DIR";
constexpr wchar_t kLocalAppData[] = L"LOCALAPPDATA";

std::mutex& title_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<Title>& title_override() {
    static std::optional<Title> value;
    return value;
}

std::optional<std::wstring> environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0 || written >= size) return std::nullopt;
    value.resize(written);
    return value;
}

std::optional<std::uint64_t> parse_decimal(std::string_view text) {
    std::size_t at = 0;
    if (!text.empty() && text.front() == '+') at = 1;
    if (at == text.size()) return std::nullopt;
    std::uint64_t value = 0;
    for (; at < text.size(); ++at) {
        const char character = text[at];
        if (character < '0' || character > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

std::filesystem::path user_data_base() {
    const std::optional<std::wstring> local = environment(kLocalAppData);
    if (local && !local->empty()) return std::filesystem::path(*local);
    std::error_code error;
    const std::filesystem::path fallback = std::filesystem::temp_directory_path(error);
    if (error) return {};
    return fallback;
}

std::filesystem::path data_folder_for(std::uint32_t id) {
    const std::optional<std::wstring> configured = settings::get_wide(kDataVariable);
    if (configured && !configured->empty()) return std::filesystem::path(*configured);
    const std::filesystem::path base = user_data_base();
    if (base.empty()) return {};
    return base / L"g6shim" / L"data" / title_identifier(id);
}

}  // namespace

std::string title_identifier(std::uint32_t id) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%08X", id);
    return buffer;
}

std::filesystem::path data_folder() { return data_folder_for(title().id); }

std::filesystem::path persistent_storage_folder() {
    const int letter = g6shim::drives::persistent_storage_drive();
    if (letter == 0) return {};
    const std::string root{static_cast<char>(letter), ':', '\\'};
    const std::optional<std::wstring> host = g6shim::drives::translate(root.c_str());
    if (!host) return {};
    return std::filesystem::path(*host);
}

Title title_from_manifest(std::string_view manifest) {
    Title value;
    value.id = xgame::manifest_title_id(manifest).value_or(0);
    value.store_id = xgame::manifest_element(manifest, kStoreIdElement).value_or("");
    const std::optional<std::string> megabytes = xgame::manifest_element(manifest, kStorageSizeElement);
    value.storage_bytes = parse_decimal(megabytes.value_or("")).value_or(0) << 20;
    return value;
}

Title title() {
    {
        std::lock_guard<std::mutex> lock(title_mutex());
        if (title_override()) return *title_override();
    }
    Title value = title_from_manifest(xgame::title_manifest().value_or(""));
    if (value.saves.empty()) value.saves = data_folder_for(value.id) / L"saves";
    return value;
}

void set_title(const Title& value) {
    std::lock_guard<std::mutex> lock(title_mutex());
    title_override() = value;
}

void clear_title() {
    std::lock_guard<std::mutex> lock(title_mutex());
    title_override().reset();
}

}  // namespace g6shim::xgameruntime
