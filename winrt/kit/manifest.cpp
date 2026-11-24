// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/winrt/manifest.hpp>
#include <g6shim/winrt/package.hpp>

#include <windows.h>

#include <fstream>

namespace g6shim::winrt {

namespace {

constexpr const wchar_t* kManifests[] = {L"AppxManifest.console.xml", L"AppxManifest.xml"};
constexpr std::string_view kServerOpen = "<InProcessServer>";
constexpr std::string_view kServerClose = "</InProcessServer>";
constexpr std::string_view kPathOpen = "<Path>";
constexpr std::string_view kPathClose = "</Path>";
constexpr std::string_view kClassId = "ActivatableClassId=\"";

}  // namespace

std::wstring game_folder() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(exe, n);
    dir.erase(dir.find_last_of(L"\\/") + 1);
    return dir;
}

const std::string& manifest_text() {
    static const std::string text = [] {
        std::string out;
        for (const wchar_t* file : kManifests) {
            std::ifstream in(game_folder() + file, std::ios::binary);
            if (!in) continue;
            out.resize(kMaxManifestBytes);
            in.read(out.data(), static_cast<std::streamsize>(out.size()));
            out.resize(static_cast<std::size_t>(in.gcount()));
            break;
        }
        return out;
    }();
    return text;
}

std::string manifest_attribute(std::string_view text, std::string_view element, std::string_view attribute) {
    const auto tag = text.find(std::string(element) + " ");
    if (tag == std::string_view::npos) return {};
    const auto end = text.find('>', tag);
    const std::string key = " " + std::string(attribute) + "=\"";
    const auto at = text.find(key, tag);
    if (at == std::string_view::npos || at > end) return {};
    const auto begin = at + key.size();
    const auto quote = text.find('"', begin);
    if (quote == std::string_view::npos || quote > end || quote - begin > kMaxAttributeValue) return {};
    return std::string(text.substr(begin, quote - begin));
}

std::map<std::string, std::string> parse_in_process_servers(std::string_view text) {
    std::map<std::string, std::string> servers;
    std::size_t pos = 0;
    for (std::size_t blocks = 0; blocks < kMaxInProcessServers; ++blocks) {
        const std::size_t begin = text.find(kServerOpen, pos);
        if (begin == std::string_view::npos) break;
        const std::size_t end = text.find(kServerClose, begin);
        if (end == std::string_view::npos) break;
        pos = end;
        const std::size_t path_at = text.find(kPathOpen, begin);
        const std::size_t path_end = text.find(kPathClose, begin);
        if (path_at == std::string_view::npos || path_end == std::string_view::npos || path_end > end ||
            path_at + kPathOpen.size() > path_end)
            continue;
        const std::string path(text.substr(path_at + kPathOpen.size(), path_end - path_at - kPathOpen.size()));
        if (!is_safe_package_path(path)) continue;
        for (std::size_t at = text.find(kClassId, begin); at != std::string_view::npos && at < end;
             at = text.find(kClassId, at + 1)) {
            const std::size_t name_at = at + kClassId.size();
            const std::size_t quote = text.find('"', name_at);
            if (quote == std::string_view::npos || quote > end || quote - name_at > kMaxClassName) continue;
            servers[std::string(text.substr(name_at, quote - name_at))] = path;
        }
    }
    return servers;
}

bool is_safe_package_path(std::string_view path) {
    if (path.empty() || path.size() > MAX_PATH / 2) return false;
    if (path.find(':') != std::string_view::npos || path.find("..") != std::string_view::npos) return false;
    return path[0] != '\\' && path[0] != '/';
}

}  // namespace g6shim::winrt
