// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace g6shim::relink {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Options {
    std::map<std::string, std::string> renames;
    bool fix_subsystem = true;
};

struct Result {
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> changes;
    std::vector<std::string> warnings;
};

Result relink(std::span<const std::uint8_t> input, const Options& options);

std::string default_new_name(const std::string& dll);

inline constexpr char kSectionName[] = ".g6shim";

}  // namespace g6shim::relink
