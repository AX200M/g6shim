// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xgame.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

namespace g6shim::xgameruntime::xgame {

namespace {

using This = Object*;

constexpr wchar_t kManifestFile[] = L"MicrosoftGame.config";
constexpr std::string_view kTitleIdPath = "Game/TitleId";
constexpr char kWhitespace[] = " \t\n\r\f\v";

std::string_view trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) return {};
    const std::size_t end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

std::string_view first_token(std::string_view text) {
    text = trim(text);
    const std::size_t end = text.find_first_of(kWhitespace);
    return end == std::string_view::npos ? text : text.substr(0, end);
}

std::string decode_entities(std::string body) {
    struct Entity {
        std::string_view text;
        char value;
    };
    constexpr Entity kEntities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (const Entity& entity : kEntities) {
        std::string decoded;
        decoded.reserve(body.size());
        for (std::size_t at = 0; at < body.size();) {
            if (body.compare(at, entity.text.size(), entity.text) == 0) {
                decoded.push_back(entity.value);
                at += entity.text.size();
            } else {
                decoded.push_back(body[at]);
                ++at;
            }
        }
        body = std::move(decoded);
    }
    return body;
}

std::string joined_path(const std::vector<std::string>& path) {
    std::string joined;
    for (const std::string& name : path) {
        if (!joined.empty()) joined.push_back('/');
        joined += name;
    }
    return joined;
}

std::optional<std::string> element_text(std::string_view text, std::string_view wanted) {
    std::vector<std::string> path;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t open = text.find('<', pos);
        if (open == std::string_view::npos) break;
        std::string_view rest = text.substr(open);
        std::size_t end = 0;
        if (rest.starts_with("<!--")) {
            const std::size_t close = rest.find("-->");
            if (close == std::string_view::npos) break;
            end = close + 3;
        } else if (rest.starts_with("<![CDATA[")) {
            const std::size_t close = rest.find("]]>");
            if (close == std::string_view::npos) break;
            end = close + 3;
        } else {
            const std::size_t close = rest.find('>');
            if (close == std::string_view::npos) break;
            end = close + 1;
        }
        const std::string_view tag = rest.substr(1, end - 2);
        rest.remove_prefix(end);
        pos = open + end;
        if (!tag.empty() && (tag.front() == '!' || tag.front() == '?')) continue;
        if (!tag.empty() && tag.front() == '/') {
            const std::string name(trim(tag.substr(1)));
            for (std::size_t index = path.size(); index-- > 0;) {
                if (path[index] == name) {
                    path.resize(index);
                    break;
                }
            }
            continue;
        }
        bool closed = false;
        std::string_view body = tag;
        while (body.ends_with('/')) {
            body.remove_suffix(1);
            closed = true;
        }
        path.emplace_back(first_token(body));
        const std::size_t body_end = rest.find('<');
        const std::string value = decode_entities(std::string(trim(rest.substr(0, body_end))));
        if (joined_path(path) == wanted) return value;
        if (closed) path.pop_back();
    }
    return std::nullopt;
}

std::optional<std::uint32_t> parse_hex(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t at = text.front() == '+' ? 1 : 0;
    if (at == text.size()) return std::nullopt;
    std::uint32_t value = 0;
    for (; at < text.size(); ++at) {
        const char character = text[at];
        std::uint32_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint32_t>(character - 'a') + 10;
        } else if (character >= 'A' && character <= 'F') {
            digit = static_cast<std::uint32_t>(character - 'A') + 10;
        } else {
            return std::nullopt;
        }
        if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 16) return std::nullopt;
        value = value * 16 + digit;
    }
    return value;
}

std::wstring executable_folder() {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(module, length);
    path.erase(path.find_last_of(L"\\/") + 1);
    return path;
}

HResult title_id_method(This, std::uint32_t* out) { return put(out, title_id()); }

void launch_new_game(This, const char* exe, const char* args, const void*) {
    const std::string executable = text(exe).value_or("");
    const std::string arguments = text(args).value_or("");
    G6SHIM_INFO("xgameruntime: XLaunchNewGame exe=%s args=%s", executable.c_str(), arguments.c_str());
}

bool is_feature_available(This, std::uint32_t feature) {
    return std::find(kAvailableFeatures.begin(), kAvailableFeatures.end(), feature) != kAvailableFeatures.end();
}

HResult register_event(This, const void*, void*, const void*, std::uint64_t* out) { return put(out, token()); }

bool unregister_event(This, std::uint64_t, bool) { return true; }

HResult write_event(This, const void*, const char*, const char*, const char* name, const char*, const char*) {
    if (name != nullptr) G6SHIM_TRACE("xgameruntime: game event dropped: %s", name);
    return kOk;
}

const std::array<const void*, 5> kTitleVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&title_id_method), slot(&launch_new_game)};

const std::array<const void*, 4> kFeaturesVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&is_feature_available)};

const std::array<const void*, 5> kInvitesVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&register_event), slot(&unregister_event)};

const std::array<const void*, 4> kEventsVtable = {
    slot(&missing<0>), slot(&add_ref), slot(&add_ref), slot(&write_event)};

}  // namespace

const Object kTitleObject{kTitleVtable.data(), "title"};
const Object kFeaturesObject{kFeaturesVtable.data(), "features"};
const Object kInvitesObject{kInvitesVtable.data(), "invites"};
const Object kEventsObject{kEventsVtable.data(), "events"};

std::optional<std::uint32_t> manifest_title_id(std::string_view text_value) {
    const std::optional<std::string> value = element_text(text_value, kTitleIdPath);
    if (!value) return std::nullopt;
    return parse_hex(trim(*value));
}

std::optional<std::string> manifest_element(std::string_view text_value, std::string_view path) {
    return element_text(text_value, path);
}

std::optional<std::string> title_manifest() {
    static const std::optional<std::string> content = []() -> std::optional<std::string> {
        std::wstring path = executable_folder();
        if (path.empty()) return std::nullopt;
        path += kManifestFile;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::string text(kMaxManifestBytes, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        return text;
    }();
    return content;
}

std::uint32_t title_id() {
    static const std::uint32_t value = [] {
        const std::optional<std::string> content = title_manifest();
        if (!content) return std::uint32_t{0};
        return manifest_title_id(*content).value_or(0);
    }();
    return value;
}

std::vector<std::string> relaunch_args(const std::vector<std::string>& ours, std::string_view current,
                                       std::string_view exe, std::string_view args) {
    std::vector<std::string> out;
    for (const std::string& argument : ours) {
        if (argument == "--") break;
        out.push_back(argument);
    }
    if (!exe.empty()) {
        const auto found = std::find(out.begin(), out.end(), std::string(current));
        if (found != out.end()) *found = std::string(exe);
    }
    if (!trim(args).empty()) {
        out.emplace_back("--");
        std::size_t at = 0;
        while (at < args.size()) {
            at = args.find_first_not_of(kWhitespace, at);
            if (at == std::string_view::npos) break;
            const std::size_t end = args.find_first_of(kWhitespace, at);
            out.emplace_back(args.substr(at, end == std::string_view::npos ? args.size() - at : end - at));
            at = end;
        }
    }
    return out;
}

std::span<const Interface> interfaces() {
    static const Interface entries[] = {
        {kTitleProvider, kTitleIid, &kTitleObject},
        {kFeatures, kFeatures, &kFeaturesObject},
        {kInvites, kInvites, &kInvitesObject},
        {kEvents, kEvents, &kEventsObject},
    };
    return entries;
}

}  // namespace g6shim::xgameruntime::xgame

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xgame_interfaces() {
    return xgame::interfaces();
}
