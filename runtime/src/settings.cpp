// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/settings.hpp"

#include "settings_parse.hpp"

#include <algorithm>
#include <windows.h>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::settings {

namespace {

constexpr std::size_t kMaxModuleChars = 0x8000;
constexpr DWORD kEmptyEnvironment = 1;

std::mutex g_mutex;
bool g_loaded = false;
bool g_hold_warnings = false;
Ini g_ini;
std::vector<std::string> g_pending;

void note(std::string message) {
    if (g_pending.size() > kMaxWarnings) return;
    if (g_pending.size() == kMaxWarnings) {
        g_pending.emplace_back(std::string("settings: further problems in ") + kFileName + " were omitted");
        return;
    }
    for (const std::string& existing : g_pending)
        if (existing == message) return;
    g_pending.push_back(std::move(message));
}

std::optional<std::wstring> executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::nullopt;
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= kMaxModuleChars) return std::nullopt;
        path.resize(std::min(path.size() * 2, kMaxModuleChars));
    }
}

std::optional<std::wstring> configured_file() {
    const std::optional<std::wstring> module = executable_path();
    if (!module) return std::nullopt;
    return settings_file_next_to(*module);
}

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    const std::optional<std::wstring> path = configured_file();
    if (!path) {
        note(std::string("settings: executable path is not available; ") + kFileName + " was not read");
        return;
    }
    const FileBytes file = read_settings_file(std::filesystem::path(*path));
    if (file.missing) return;
    if (file.too_large) {
        note(std::string("settings: ") + kFileName + " is larger than " + std::to_string(kMaxFileBytes) +
             " bytes; ignoring it");
        return;
    }
    if (file.unreadable) {
        note(std::string("settings: ") + kFileName + " could not be read");
        return;
    }
    g_ini = parse_ini(file.bytes);
    for (const std::string& warning : g_ini.warnings) note(warning);
}

std::wstring widen_ascii(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (unsigned char c : text) out.push_back(c);
    return out;
}

std::optional<std::wstring> environment_wide(std::string_view variable) {
    if (variable.empty() || variable.size() > kMaxVariableChars) return std::nullopt;
    const std::wstring name = widen_ascii(variable);
    const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (needed <= kEmptyEnvironment || needed > kMaxWideChars + 1) return std::nullopt;
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
    if (written == 0 || written >= needed) return std::nullopt;
    value.resize(written);
    if (value.empty()) return std::nullopt;
    return value;
}

std::optional<std::string> utf8_from_wide(std::wstring_view text) {
    if (text.empty() || text.size() > kMaxWideChars) return std::nullopt;
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0 || static_cast<std::size_t>(length) > kMaxValueBytes) return std::nullopt;
    std::string out(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                            length, nullptr, nullptr) != length)
        return std::nullopt;
    return out;
}

std::optional<std::wstring> wide_from_utf8(std::string_view text) {
    if (text.empty() || text.size() > kMaxValueBytes) return std::nullopt;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0 || static_cast<std::size_t>(length) > kMaxWideChars) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(),
                            length) != length)
        return std::nullopt;
    return out;
}

std::optional<std::string> file_value(std::string_view variable) {
    const Binding* binding = binding_for(variable);
    if (binding == nullptr) return std::nullopt;
    const std::string* value = find_value(g_ini, binding->section, binding->key);
    if (value == nullptr || value->empty()) return std::nullopt;
    return *value;
}

std::optional<std::string> resolved(std::string_view variable) {
    ensure_loaded();
    if (const std::optional<std::wstring> environment = environment_wide(variable)) return utf8_from_wide(*environment);
    return file_value(variable);
}

std::optional<std::wstring> resolved_wide(std::string_view variable) {
    ensure_loaded();
    if (const std::optional<std::wstring> environment = environment_wide(variable)) return environment;
    const std::optional<std::string> file = file_value(variable);
    if (!file) return std::nullopt;
    return wide_from_utf8(*file);
}

void copy_out(const void* from, std::size_t bytes, void* buffer, std::size_t buffer_bytes) {
    if (buffer == nullptr || buffer_bytes < bytes || from == nullptr || bytes == 0) return;
    std::char_traits<char>::copy(static_cast<char*>(buffer), static_cast<const char*>(from), bytes);
}

}  // namespace

void hold_warnings(bool hold) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_warnings = hold;
}

void flush_warnings() {
    std::vector<std::string> local;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hold_warnings) return;
        local.swap(g_pending);
    }
    for (const std::string& message : local) log(Level::Warn, "%s", message.c_str());
}

std::size_t copy(const char* variable, std::size_t variable_length, char* buffer, std::size_t buffer_size) {
    if (variable == nullptr || variable_length == 0 || variable_length > kMaxVariableChars) return 0;
    std::optional<std::string> value;
    bool flush = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        value = resolved(std::string_view(variable, variable_length));
        flush = !g_hold_warnings && !g_pending.empty();
    }
    if (flush) flush_warnings();
    if (!value || value->empty() || value->size() > kMaxValueBytes) return 0;
    if (buffer == nullptr || buffer_size < value->size()) return value->size();
    copy_out(value->data(), value->size(), buffer, buffer_size);
    return value->size();
}

std::size_t copy_wide(const char* variable, std::size_t variable_length, wchar_t* buffer, std::size_t buffer_size) {
    if (variable == nullptr || variable_length == 0 || variable_length > kMaxVariableChars) return 0;
    std::optional<std::wstring> value;
    bool flush = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        value = resolved_wide(std::string_view(variable, variable_length));
        flush = !g_hold_warnings && !g_pending.empty();
    }
    if (flush) flush_warnings();
    if (!value || value->empty() || value->size() > kMaxWideChars) return 0;
    if (buffer == nullptr || buffer_size < value->size()) return value->size();
    copy_out(value->data(), value->size() * sizeof(wchar_t), buffer, buffer_size * sizeof(wchar_t));
    return value->size();
}

void install_bytes_for_test(const std::uint8_t* bytes, std::size_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = true;
    g_pending.clear();
    if (bytes == nullptr || size == 0) {
        g_ini = {};
        return;
    }
    g_ini = parse_ini(std::span<const std::uint8_t>(bytes, size));
    for (const std::string& warning : g_ini.warnings) note(warning);
}

void reload_file_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded = false;
    g_ini = {};
    g_pending.clear();
}

std::size_t copy_configured_file(wchar_t* buffer, std::size_t buffer_size) {
    const std::optional<std::wstring> path = configured_file();
    if (!path || path->empty() || path->size() > kMaxWideChars) return 0;
    if (buffer == nullptr || buffer_size < path->size()) return path->size();
    copy_out(path->data(), path->size() * sizeof(wchar_t), buffer, buffer_size * sizeof(wchar_t));
    return path->size();
}

}  // namespace g6shim::settings
