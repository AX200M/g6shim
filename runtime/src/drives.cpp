// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/drives.hpp>

#include <g6shim/log.hpp>

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::drives {

namespace {

constexpr wchar_t kVerbatim[] = L"\\\\?\\";
constexpr std::size_t kVerbatimLength = 4;
constexpr wchar_t kBackslash = L'\\';
constexpr wchar_t kColon = L':';
constexpr int kFirstFreeLetter = L'D';
constexpr std::size_t kMaxModulePath = 0x8000;
constexpr wchar_t kPersistentFolder[] = L"g6shim_persistent";

struct Mount {
    wchar_t letter;
    std::wstring root;
};

std::mutex& table_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<Mount>& mounts() {
    static std::vector<Mount> entries;
    return entries;
}

bool separator(wchar_t c) { return c == L'\\' || c == L'/'; }

Mount* find_mount(wchar_t letter) {
    for (auto& entry : mounts())
        if (entry.letter == letter) return &entry;
    return nullptr;
}

std::wstring executable_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() >= kMaxModulePath) return {};
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash);
    return path;
}

std::wstring root_wide(const wchar_t* root) {
    if (root == nullptr || root[0] == L'\0') return {};
    std::wstring full;
    const DWORD needed = GetFullPathNameW(root, 0, nullptr, nullptr);
    if (needed > 0) {
        full.resize(needed);
        const DWORD written = GetFullPathNameW(root, needed, full.data(), nullptr);
        if (written > 0 && written < needed)
            full.resize(written);
        else
            full.assign(root);
    } else {
        full.assign(root);
    }
    while (full.size() > kDrivePrefix && separator(full.back())) full.pop_back();
    return full;
}

void set_mount(wchar_t letter, std::wstring root) {
    mounts().erase(std::remove_if(mounts().begin(), mounts().end(),
                                  [letter](const Mount& entry) { return entry.letter == letter; }),
                   mounts().end());
    mounts().push_back({letter, std::move(root)});
}

void ensure_default_mount() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::wstring folder = executable_folder();
        if (folder.empty()) return;
        {
            std::lock_guard<std::mutex> lock(table_mutex());
            if (find_mount(kPackageDrive) != nullptr) return;
            set_mount(kPackageDrive, folder);
        }
        G6SHIM_INFO("drives: %c: is %ls", kPackageDrive, folder.c_str());
    });
}

std::optional<std::wstring> verbatim_join(const std::wstring& root, std::wstring_view tail) {
    std::wstring out;
    if (root.compare(0, kVerbatimLength, kVerbatim) == 0) {
        out = root;
    } else if (root.size() >= 2 && root[0] == kBackslash && root[1] == kBackslash) {
        out.assign(kVerbatim);
        out.append(L"UNC\\");
        out.append(root, 2, std::wstring::npos);
    } else {
        out.assign(kVerbatim);
        out.append(root);
    }
    while (!out.empty() && out.back() == kBackslash) out.pop_back();
    std::vector<std::wstring_view> parts;
    std::size_t at = 0;
    while (at <= tail.size()) {
        std::size_t end = at;
        while (end < tail.size() && !separator(tail[end])) ++end;
        const std::wstring_view part = tail.substr(at, end - at);
        if (!part.empty() && part != std::wstring_view(L".")) {
            if (part == std::wstring_view(L"..")) {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
        if (end >= tail.size()) break;
        at = end + 1;
    }
    for (const auto& part : parts) {
        out.push_back(kBackslash);
        out.append(part);
    }
    return out;
}

std::optional<std::wstring> translate_onto(const std::wstring& path, const std::wstring& root) {
    if (root.empty()) return std::nullopt;
    std::size_t start = 0;
    bool verbatim = false;
    if (path.size() >= kVerbatimLength && path.compare(0, kVerbatimLength, kVerbatim) == 0) {
        start = kVerbatimLength;
        verbatim = true;
    }
    if (path.size() < start + 2) return std::nullopt;
    const wchar_t letter = upper_letter(path[start]);
    if (letter < L'A' || letter > L'Z' || path[start + 1] != kColon) return std::nullopt;
    const std::wstring_view tail = std::wstring_view(path).substr(start + 2);
    if (!verbatim && root.size() + tail.size() >= kLongPath) return verbatim_join(root, tail);
    std::wstring out;
    if (verbatim) out.assign(kVerbatim);
    out.append(root);
    if (tail.empty() || !separator(tail.front())) out.push_back(kBackslash);
    for (const wchar_t c : tail) out.push_back(verbatim && c == L'/' ? kBackslash : c);
    return out;
}

wchar_t drive_letter(const std::wstring& path) {
    std::size_t start = 0;
    if (path.size() >= kVerbatimLength && path.compare(0, kVerbatimLength, kVerbatim) == 0)
        start = kVerbatimLength;
    if (path.size() < start + 2 || path[start + 1] != kColon) return 0;
    const wchar_t letter = upper_letter(path[start]);
    return letter >= L'A' && letter <= L'Z' ? letter : 0;
}

std::optional<std::wstring> translate_table(const std::wstring& path) {
    ensure_default_mount();
    const wchar_t letter = drive_letter(path);
    if (letter == 0) return std::nullopt;
    std::wstring root;
    {
        std::lock_guard<std::mutex> lock(table_mutex());
        const Mount* const found = find_mount(letter);
        if (found == nullptr) return std::nullopt;
        root = found->root;
    }
    return translate_onto(path, root);
}

std::optional<std::wstring> narrow_path(const char* path) {
    if (path == nullptr) return std::nullopt;
    const std::size_t length = std::strlen(path);
    if (length > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
    const std::string_view bytes(path, length);
    std::string_view candidate = bytes;
    if (candidate.size() >= 4 && candidate.compare(0, 4, "\\\\?\\") == 0) candidate.remove_prefix(4);
    if (candidate.size() < 2 || candidate[1] != ':' ||
        !((candidate[0] >= 'a' && candidate[0] <= 'z') || (candidate[0] >= 'A' && candidate[0] <= 'Z')))
        return std::nullopt;
    const int count = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(length), nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(length), wide.data(), count);
    return translate_table(wide);
}

std::wstring wide_path(const wchar_t* path) { return path != nullptr ? std::wstring(path) : std::wstring{}; }

void copy_out(const std::optional<std::wstring>& host, wchar_t* out) {
    if (!host || out == nullptr) return;
    std::memcpy(out, host->c_str(), (host->size() + 1) * sizeof(wchar_t));
}

std::optional<std::wstring> package_join(const std::wstring& path, const std::wstring& root) {
    if (path.size() < kDrivePrefix || path[1] != kColon) return std::nullopt;
    const wchar_t letter = upper_letter(path[0]);
    if (letter != kPackageDrive || !separator(path[2])) return std::nullopt;
    if (root.empty()) return std::nullopt;
    std::wstring out = root;
    if (!separator(out.back())) out.push_back(kBackslash);
    out.append(path, kDrivePrefix, std::wstring::npos);
    return out;
}

}  // namespace

std::size_t host_path_size(const wchar_t* path) {
    if (path == nullptr) return 0;
    const auto host = translate_table(wide_path(path));
    return host ? host->size() + 1 : 0;
}

void host_path(const wchar_t* path, wchar_t* out) {
    if (path == nullptr || out == nullptr) return;
    copy_out(translate_table(wide_path(path)), out);
}

std::size_t host_narrow_path_size(const char* path) {
    const auto host = narrow_path(path);
    return host ? host->size() + 1 : 0;
}

void host_narrow_path(const char* path, wchar_t* out) { copy_out(narrow_path(path), out); }

std::size_t package_path_size(const wchar_t* path, const wchar_t* root) {
    if (path == nullptr || root == nullptr) return 0;
    const std::wstring full_root = root_wide(root);
    if (full_root.empty()) return 0;
    const auto host = package_join(wide_path(path), full_root);
    return host ? host->size() + 1 : 0;
}

void package_path(const wchar_t* path, const wchar_t* root, wchar_t* out) {
    if (path == nullptr || root == nullptr || out == nullptr) return;
    const std::wstring full_root = root_wide(root);
    if (full_root.empty()) return;
    copy_out(package_join(wide_path(path), full_root), out);
}

int mount_drive(const wchar_t* root) {
    ensure_default_mount();
    const std::wstring wide = root_wide(root);
    if (wide.empty()) return 0;
    std::lock_guard<std::mutex> lock(table_mutex());
    for (const auto& entry : mounts())
        if (entry.root == wide) return entry.letter;
    const DWORD host = GetLogicalDrives();
    for (int letter = L'Z'; letter >= kFirstFreeLetter; --letter) {
        const unsigned bit = static_cast<unsigned>(letter - L'A');
        if (bit >= 32 || ((host >> bit) & 1u) != 0) continue;
        if (find_mount(static_cast<wchar_t>(letter)) != nullptr) continue;
        set_mount(static_cast<wchar_t>(letter), wide);
        G6SHIM_INFO("drives: %c: is %ls", static_cast<wchar_t>(letter), wide.c_str());
        return letter;
    }
    G6SHIM_WARN("drives: no free letter for %ls", wide.c_str());
    return 0;
}

int persistent_storage_drive() {
    const std::wstring folder = executable_folder();
    if (folder.empty()) return 0;
    const std::wstring root = folder + kBackslash + kPersistentFolder;
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            G6SHIM_WARN("drives: cannot create %ls (error %lu)", root.c_str(), error);
            return 0;
        }
    }
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        G6SHIM_WARN("drives: %ls is not a folder", root.c_str());
        return 0;
    }
    return mount_drive(root.c_str());
}

void mount_drive_at(wchar_t letter, const wchar_t* root) {
    ensure_default_mount();
    const wchar_t upper = upper_letter(letter);
    if (upper < L'A' || upper > L'Z') return;
    const std::wstring wide = root_wide(root);
    if (wide.empty()) return;
    const unsigned index = static_cast<unsigned>(upper - L'A');
    const bool hides_host_drive = index < 32 && ((GetLogicalDrives() >> index) & 1u) != 0;
    {
        std::lock_guard<std::mutex> lock(table_mutex());
        set_mount(upper, wide);
    }
    G6SHIM_INFO("drives: %c: is %ls%s", upper, wide.c_str(), hides_host_drive ? " (hides a host drive)" : "");
}

}  // namespace g6shim::drives
