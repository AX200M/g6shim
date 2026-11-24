// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace g6shim::pe {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace layout {
inline constexpr std::uint64_t kDosSignature = 0x5a4d;
inline constexpr std::uint64_t kDosPeOffset = 0x3c;
inline constexpr std::uint32_t kPeSignature = 0x00004550;
inline constexpr std::uint64_t kPeSignatureSize = 4;

inline constexpr std::uint64_t kCoffMachine = 0;
inline constexpr std::uint64_t kCoffSectionCount = 2;
inline constexpr std::uint64_t kCoffOptionalHeaderSize = 16;
inline constexpr std::uint64_t kCoffCharacteristics = 18;
inline constexpr std::uint64_t kCoffHeaderSize = 20;

inline constexpr std::uint16_t kMagicPe32 = 0x10b;
inline constexpr std::uint16_t kMagicPe32Plus = 0x20b;
inline constexpr std::uint64_t kOptMagic = 0;
inline constexpr std::uint64_t kOptInitializedDataSize = 8;
inline constexpr std::uint64_t kOptEntryPoint = 16;
inline constexpr std::uint64_t kOptImageBase32 = 28;
inline constexpr std::uint64_t kOptImageBase64 = 24;
inline constexpr std::uint64_t kOptSectionAlignment = 32;
inline constexpr std::uint64_t kOptFileAlignment = 36;
inline constexpr std::uint64_t kOptSizeOfImage = 56;
inline constexpr std::uint64_t kOptSizeOfHeaders = 60;
inline constexpr std::uint64_t kOptCheckSum = 64;
inline constexpr std::uint64_t kOptSubsystem = 68;
inline constexpr std::uint64_t kOptDllCharacteristics = 70;
inline constexpr std::uint64_t kOptDirectoryCount32 = 92;
inline constexpr std::uint64_t kOptDirectoryCount64 = 108;
inline constexpr std::uint64_t kOptDirectories32 = 96;
inline constexpr std::uint64_t kOptDirectories64 = 112;
inline constexpr std::uint32_t kMaxDirectories = 16;
inline constexpr std::uint64_t kDirectorySize = 8;

inline constexpr std::uint64_t kSectionHeaderSize = 40;
inline constexpr std::uint64_t kSectionNameSize = 8;
inline constexpr std::uint64_t kSectionVirtualSize = 8;
inline constexpr std::uint64_t kSectionVirtualAddress = 12;
inline constexpr std::uint64_t kSectionRawSize = 16;
inline constexpr std::uint64_t kSectionRawOffset = 20;
inline constexpr std::uint64_t kSectionCharacteristics = 36;

inline constexpr std::uint32_t kDirExport = 0;
inline constexpr std::uint32_t kDirImport = 1;
inline constexpr std::uint32_t kDirSecurity = 4;
inline constexpr std::uint32_t kDirBoundImport = 11;
inline constexpr std::uint32_t kDirDelayImport = 13;

inline constexpr std::uint64_t kImportDescriptorSize = 20;
inline constexpr std::uint64_t kImportTimestamp = 4;
inline constexpr std::uint64_t kImportName = 12;
inline constexpr std::uint64_t kDelayDescriptorSize = 32;
inline constexpr std::uint64_t kDelayName = 4;
inline constexpr std::uint32_t kDelayRvaBased = 1;

inline constexpr std::uint16_t kCharacteristicDll = 0x2000;
inline constexpr std::uint16_t kSubsystemWindowsGui = 2;
inline constexpr std::uint16_t kSubsystemXbox = 14;
}  // namespace layout

struct Section {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
};

struct Import {
    std::string name;
    std::optional<std::uint16_t> ordinal;
    std::uint16_t hint = 0;

    std::string display() const;
};

struct ImportModule {
    std::string dll;
    bool delay_loaded = false;
    std::vector<Import> functions;
    std::uint32_t descriptor_rva = 0;
    bool name_is_va = false;
};

struct DataDirectory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct Export {
    std::string name;
    std::uint32_t ordinal = 0;
    std::uint32_t rva = 0;
    std::string forwarder;
};

struct Image {
    std::uint16_t machine = 0;
    bool pe32_plus = false;
    std::uint64_t image_base = 0;
    std::uint32_t entry_point_rva = 0;
    std::uint16_t subsystem = 0;
    std::uint16_t characteristics = 0;
    std::vector<Section> sections;
    std::vector<ImportModule> imports;
    std::string export_name;
    std::vector<Export> exports;
    std::vector<std::string> warnings;

    std::uint64_t coff_header_offset = 0;
    std::uint64_t optional_header_offset = 0;
    std::uint64_t section_table_offset = 0;
    std::uint64_t data_directory_offset = 0;
    std::uint32_t section_alignment = 0;
    std::uint32_t file_alignment = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint16_t dll_characteristics = 0;
    std::vector<DataDirectory> directories;

    bool is_dll() const { return (characteristics & layout::kCharacteristicDll) != 0; }

    std::uint64_t offset_of(std::uint64_t rva) const;
};

Image parse(std::span<const std::uint8_t> data);
Image parse_file(const std::filesystem::path& path);

std::string machine_name(std::uint16_t machine);
std::string subsystem_name(std::uint16_t subsystem);
std::string to_lower_ascii(std::string s);
std::string hex(std::uint64_t value);

}  // namespace g6shim::pe
