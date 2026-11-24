// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g6shim::hle {

enum class Status { Missing, Stubbed, Implemented };

struct DefExport {
    std::string entry;
    std::string internal;
    std::optional<std::uint16_t> ordinal;
    bool noname = false;
    bool data = false;

    std::string key() const;
    bool is_stub() const;
};

struct DefFile {
    std::string dll;
    std::vector<DefExport> exports;

    static DefFile parse(std::string_view text, const std::string& fallback_dll);
    static DefFile load(const std::filesystem::path& path);
};

class HleTree {
public:
    static HleTree load(const std::filesystem::path& root);

    Status status(std::string_view dll, std::string_view key) const;
    bool has_module(std::string_view dll) const;
    std::size_t module_count() const { return index_.size(); }

private:
    std::map<std::string, std::map<std::string, bool>> index_;
};

bool is_def_safe_name(std::string_view name);

inline constexpr std::string_view kStubPrefix = "g6shim_stub_";
inline constexpr std::string_view kStubMacro = "G6SHIM_STUB(";

std::size_t next_stub_id(const DefFile& def, std::string_view stub_source);

}  // namespace g6shim::hle
