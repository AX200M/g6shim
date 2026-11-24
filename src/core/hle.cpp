// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/hle.hpp"
#include "g6shim/pe.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace g6shim::hle {

namespace {

constexpr std::size_t kMaxDefName = 1024;

}  // namespace

std::string DefExport::key() const {
    if (noname && ordinal) return "#" + std::to_string(*ordinal);
    return entry;
}

bool DefExport::is_stub() const {
    const std::string& target = internal.empty() ? entry : internal;
    return target.rfind(kStubPrefix, 0) == 0;
}

bool is_def_safe_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxDefName) return false;
    if (name.front() == '@' || name.front() == '"') return false;
    for (unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7f) return false;
        if (c == ';' || c == '=' || c == ',' || c == '"') return false;
    }
    return true;
}

std::size_t next_stub_id(const DefFile& def, std::string_view stub_source) {
    std::size_t next = 0;
    auto consider = [&](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        std::uint32_t value = 0;
        const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec == std::errc{} && value < std::numeric_limits<std::uint32_t>::max())
            next = std::max<std::size_t>(next, value + 1);
    };
    for (const auto& ex : def.exports) {
        const std::string_view target = ex.internal.empty() ? ex.entry : ex.internal;
        if (target.rfind(kStubPrefix, 0) == 0) consider(target.substr(kStubPrefix.size()));
    }
    for (auto p = stub_source.find(kStubMacro); p != std::string_view::npos; p = stub_source.find(kStubMacro, p + 1))
        consider(stub_source.substr(p + kStubMacro.size()));
    return next;
}

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return std::string(s);
}

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> raw;
    std::istringstream in{std::string(line)};
    for (std::string t; in >> t;) raw.push_back(t);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == "=" && !out.empty() && i + 1 < raw.size()) {
            out.back() += "=" + raw[++i];
        } else if (!out.empty() && out.back().back() == '=') {
            out.back() += raw[i];
        } else if (raw[i].front() == '=' && !out.empty()) {
            out.back() += raw[i];
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

std::optional<std::uint16_t> parse_ordinal(std::string_view s) {
    unsigned value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size() || value == 0 || value > 0xffff) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

bool ends_exports(const std::string& head) {
    return head == "HEAPSIZE" || head == "STACKSIZE" || head == "SECTIONS" || head == "VERSION" ||
           head == "DESCRIPTION" || head == "STUB";
}

}  // namespace

DefFile DefFile::parse(std::string_view text, const std::string& fallback_dll) {
    DefFile def;
    std::string library;
    bool in_exports = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;

        if (auto c = line.find(';'); c != std::string_view::npos) line = line.substr(0, c);
        auto tokens = tokenize(trim(line));
        if (tokens.empty()) continue;

        const std::string head = upper(tokens[0]);
        if (head == "EXPORTS") {
            in_exports = true;
            tokens.erase(tokens.begin());
            if (tokens.empty()) continue;
        } else if (head == "LIBRARY" || head == "NAME") {
            if (tokens.size() > 1) library = unquote(tokens[1]);
            in_exports = false;
            continue;
        } else if (ends_exports(head)) {
            in_exports = false;
            continue;
        }
        if (!in_exports) continue;

        DefExport ex;
        const std::string& first = tokens[0];
        if (auto eq = first.find('='); eq != std::string::npos) {
            ex.entry = first.substr(0, eq);
            ex.internal = first.substr(eq + 1);
        } else {
            ex.entry = first;
        }
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            std::string t = tokens[i];
            if (t == "@" && i + 1 < tokens.size()) t += tokens[++i];
            const std::string u = upper(t);
            if (!t.empty() && t[0] == '@') ex.ordinal = parse_ordinal(std::string_view(t).substr(1));
            else if (u == "NONAME") ex.noname = true;
            else if (u == "DATA") ex.data = true;
        }
        def.exports.push_back(std::move(ex));
    }

    def.dll = library.empty() ? fallback_dll : library;
    return def;
}

DefFile DefFile::load(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), path.stem().string() + ".dll");
}

HleTree HleTree::load(const fs::path& root) {
    HleTree tree;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return tree;
    for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || pe::to_lower_ascii(entry.path().extension().string()) != ".def") continue;
        const DefFile def = DefFile::load(entry.path());
        auto& funcs = tree.index_[pe::to_lower_ascii(def.dll)];
        for (const auto& ex : def.exports) funcs[ex.key()] = ex.is_stub();
    }
    return tree;
}

Status HleTree::status(std::string_view dll, std::string_view key) const {
    auto m = index_.find(pe::to_lower_ascii(std::string(dll)));
    if (m == index_.end()) return Status::Missing;
    auto f = m->second.find(std::string(key));
    if (f == m->second.end()) return Status::Missing;
    return f->second ? Status::Stubbed : Status::Implemented;
}

bool HleTree::has_module(std::string_view dll) const {
    return index_.count(pe::to_lower_ascii(std::string(dll))) != 0;
}

}  // namespace g6shim::hle
