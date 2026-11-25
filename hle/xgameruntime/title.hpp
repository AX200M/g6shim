// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace g6shim::xgameruntime {

struct Title {
    std::uint32_t id = 0;
    std::string store_id;
    std::uint64_t storage_bytes = 0;
    std::filesystem::path storage;
    std::filesystem::path saves;
};

Title title();
Title title_from_manifest(std::string_view manifest);
void set_title(const Title& value);
void clear_title();

std::string title_identifier(std::uint32_t id);
std::filesystem::path data_folder();
std::filesystem::path persistent_storage_folder();

}  // namespace g6shim::xgameruntime
