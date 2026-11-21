// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace g6shim::test {

inline constexpr std::size_t kSampleCoff = 0x44;
inline constexpr std::size_t kSampleOptional = 0x58;
inline constexpr std::size_t kSampleDirectories = kSampleOptional + 112;
inline constexpr std::size_t kSampleSections = kSampleOptional + 240;
inline constexpr std::size_t kSampleHeaders = 0x200;

std::vector<std::uint8_t> build_sample_pe();

template <class T>
void put(std::vector<std::uint8_t>& b, std::size_t off, T v) {
    std::memcpy(&b[off], &v, sizeof v);
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);

struct TempDir {
    std::filesystem::path path;
    TempDir();
    ~TempDir();
};

}  // namespace g6shim::test
