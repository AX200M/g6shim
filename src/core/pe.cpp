// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "g6shim/pe.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

static_assert(std::endian::native == std::endian::little, "the PE reader assumes a little-endian host");

namespace g6shim::pe {

using namespace layout;

std::string hex(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Import::display() const {
    if (ordinal) return "#" + std::to_string(*ordinal);
    return name;
}

std::string machine_name(std::uint16_t machine) {
    switch (machine) {
        case 0x8664: return "x64";
        case 0x014c: return "x86";
        case 0xaa64: return "ARM64";
        case 0x01c4: return "ARMv7";
        default: return "unknown(" + hex(machine) + ")";
    }
}

std::string subsystem_name(std::uint16_t subsystem) {
    switch (subsystem) {
        case 1: return "Native";
        case 2: return "Windows GUI";
        case 3: return "Windows console";
        case 10: return "EFI application";
        case kSubsystemXbox: return "game console";
        case 16: return "Windows boot application";
        default: return "unknown(" + std::to_string(subsystem) + ")";
    }
}

namespace {

constexpr std::size_t kMaxString = 4096;
constexpr std::size_t kMaxModules = 4096;
constexpr std::size_t kMaxFunctionsPerModule = 65536;
constexpr std::uint32_t kMaxExports = 1u << 20;

constexpr std::uint64_t kExportNameRva = 12;
constexpr std::uint64_t kExportOrdinalBase = 16;
constexpr std::uint64_t kExportFunctionCount = 20;
constexpr std::uint64_t kExportNameCount = 24;
constexpr std::uint64_t kExportFunctions = 28;
constexpr std::uint64_t kExportNames = 32;
constexpr std::uint64_t kExportNameOrdinals = 36;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    template <class T>
    T read(std::uint64_t off) const {
        if (off > data_.size() || sizeof(T) > data_.size() - off)
            throw ParseError("read past end of file at offset " + hex(off));
        T value;
        std::memcpy(&value, data_.data() + off, sizeof(T));
        return value;
    }

    std::string cstr(std::uint64_t off) const {
        if (off >= data_.size()) throw ParseError("string offset " + hex(off) + " is past end of file");
        const auto* begin = data_.data() + off;
        const auto* limit = data_.data() + std::min<std::uint64_t>(data_.size(), off + kMaxString);
        const auto* nul = std::find(begin, limit, std::uint8_t{0});
        if (nul == limit) throw ParseError("unterminated string at offset " + hex(off));
        return std::string(reinterpret_cast<const char*>(begin), static_cast<std::size_t>(nul - begin));
    }

private:
    std::span<const std::uint8_t> data_;
};

class Parser {
public:
    explicit Parser(std::span<const std::uint8_t> data) : r_(data) {}

    Image run() {
        if (r_.read<std::uint16_t>(0) != kDosSignature) throw ParseError("not a PE file (no MZ header)");
        const std::uint32_t pe_off = r_.read<std::uint32_t>(kDosPeOffset);
        if (r_.read<std::uint32_t>(pe_off) != kPeSignature) throw ParseError("not a PE file (no PE signature)");

        const std::uint64_t coff = std::uint64_t{pe_off} + kPeSignatureSize;
        img_.machine = r_.read<std::uint16_t>(coff + kCoffMachine);
        const std::uint16_t section_count = r_.read<std::uint16_t>(coff + kCoffSectionCount);
        const std::uint16_t opt_size = r_.read<std::uint16_t>(coff + kCoffOptionalHeaderSize);
        img_.characteristics = r_.read<std::uint16_t>(coff + kCoffCharacteristics);

        const std::uint64_t opt = coff + kCoffHeaderSize;
        const std::uint16_t magic = r_.read<std::uint16_t>(opt + kOptMagic);
        if (magic == kMagicPe32Plus) img_.pe32_plus = true;
        else if (magic != kMagicPe32) throw ParseError("unknown optional header magic " + hex(magic));

        img_.coff_header_offset = coff;
        img_.optional_header_offset = opt;
        img_.entry_point_rva = r_.read<std::uint32_t>(opt + kOptEntryPoint);
        img_.image_base = img_.pe32_plus ? r_.read<std::uint64_t>(opt + kOptImageBase64)
                                         : r_.read<std::uint32_t>(opt + kOptImageBase32);
        img_.section_alignment = r_.read<std::uint32_t>(opt + kOptSectionAlignment);
        img_.file_alignment = r_.read<std::uint32_t>(opt + kOptFileAlignment);
        img_.size_of_image = r_.read<std::uint32_t>(opt + kOptSizeOfImage);
        img_.size_of_headers = r_.read<std::uint32_t>(opt + kOptSizeOfHeaders);
        img_.subsystem = r_.read<std::uint16_t>(opt + kOptSubsystem);
        img_.dll_characteristics = r_.read<std::uint16_t>(opt + kOptDllCharacteristics);

        const std::uint64_t count_off = img_.pe32_plus ? kOptDirectoryCount64 : kOptDirectoryCount32;
        const std::uint64_t dirs_off = img_.pe32_plus ? kOptDirectories64 : kOptDirectories32;
        std::uint32_t dir_count = 0;
        if (opt_size >= count_off + 4) dir_count = r_.read<std::uint32_t>(opt + count_off);
        const auto fits = static_cast<std::uint32_t>(opt_size > dirs_off ? (opt_size - dirs_off) / kDirectorySize : 0);
        dir_count = std::min({dir_count, fits, kMaxDirectories});
        img_.data_directory_offset = opt + dirs_off;
        for (std::uint32_t i = 0; i < dir_count; ++i) {
            const std::uint64_t entry = opt + dirs_off + i * kDirectorySize;
            img_.directories.push_back({r_.read<std::uint32_t>(entry), r_.read<std::uint32_t>(entry + 4)});
        }

        const std::uint64_t sections = opt + opt_size;
        img_.section_table_offset = sections;
        for (std::uint32_t i = 0; i < section_count; ++i) {
            const std::uint64_t s = sections + std::uint64_t{i} * kSectionHeaderSize;
            Section sec;
            char name[kSectionNameSize + 1] = {};
            for (std::uint64_t j = 0; j < kSectionNameSize; ++j) name[j] = static_cast<char>(r_.read<std::uint8_t>(s + j));
            sec.name = name;
            sec.virtual_size = r_.read<std::uint32_t>(s + kSectionVirtualSize);
            sec.virtual_address = r_.read<std::uint32_t>(s + kSectionVirtualAddress);
            sec.raw_size = r_.read<std::uint32_t>(s + kSectionRawSize);
            sec.raw_offset = r_.read<std::uint32_t>(s + kSectionRawOffset);
            sec.characteristics = r_.read<std::uint32_t>(s + kSectionCharacteristics);
            img_.sections.push_back(std::move(sec));
        }

        guarded("import directory", [&] { parse_imports(directory(kDirImport)); });
        guarded("delay-import directory", [&] { parse_delay_imports(directory(kDirDelayImport)); });
        guarded("export directory", [&] { parse_exports(directory(kDirExport)); });
        return std::move(img_);
    }

private:
    DataDirectory directory(std::uint32_t index) const {
        return index < img_.directories.size() ? img_.directories[index] : DataDirectory{};
    }

    template <class F>
    void guarded(const char* what, F&& fn) {
        try {
            fn();
        } catch (const ParseError& e) {
            img_.warnings.push_back(std::string(what) + ": " + e.what());
        }
    }

    std::uint64_t rva_to_offset(std::uint64_t rva) const { return img_.offset_of(rva); }

    std::uint64_t to_rva(std::uint64_t value, bool va_based) const {
        if (!va_based) return value;
        if (value < img_.image_base) throw ParseError("address " + hex(value) + " is below the image base");
        return value - img_.image_base;
    }

    std::vector<Import> read_thunks(std::uint64_t thunk_rva, bool va_based = false) {
        std::vector<Import> out;
        const std::uint64_t width = img_.pe32_plus ? 8 : 4;
        const std::uint64_t ordinal_flag = img_.pe32_plus ? (1ull << 63) : (1ull << 31);
        for (std::size_t i = 0;; ++i) {
            if (i >= kMaxFunctionsPerModule) throw ParseError("import thunk table is too long");
            const std::uint64_t off = rva_to_offset(thunk_rva + i * width);
            const std::uint64_t v = img_.pe32_plus ? r_.read<std::uint64_t>(off) : r_.read<std::uint32_t>(off);
            if (v == 0) break;
            Import imp;
            if (v & ordinal_flag) {
                imp.ordinal = static_cast<std::uint16_t>(v & 0xffff);
            } else {
                const std::uint64_t by_name = va_based ? to_rva(v, true) : (v & 0x7fffffffull);
                const std::uint64_t o = rva_to_offset(by_name);
                imp.hint = r_.read<std::uint16_t>(o);
                imp.name = r_.cstr(o + 2);
            }
            out.push_back(std::move(imp));
        }
        return out;
    }

    void parse_imports(DataDirectory d) {
        if (d.rva == 0) return;
        for (std::size_t i = 0;; ++i) {
            if (i >= kMaxModules) throw ParseError("too many import descriptors");
            const std::uint64_t e = rva_to_offset(std::uint64_t{d.rva} + i * kImportDescriptorSize);
            const auto original_first_thunk = r_.read<std::uint32_t>(e);
            const auto timestamp = r_.read<std::uint32_t>(e + kImportTimestamp);
            const auto forwarder_chain = r_.read<std::uint32_t>(e + 8);
            const auto name_rva = r_.read<std::uint32_t>(e + kImportName);
            const auto first_thunk = r_.read<std::uint32_t>(e + 16);
            if (!original_first_thunk && !timestamp && !forwarder_chain && !name_rva && !first_thunk) break;

            ImportModule m;
            m.descriptor_rva = static_cast<std::uint32_t>(d.rva + i * kImportDescriptorSize);
            m.dll = r_.cstr(rva_to_offset(name_rva));
            m.functions = read_thunks(original_first_thunk ? original_first_thunk : first_thunk);
            img_.imports.push_back(std::move(m));
        }
    }

    void parse_delay_imports(DataDirectory d) {
        if (d.rva == 0) return;
        const std::size_t declared = d.size >= kDelayDescriptorSize ? d.size / kDelayDescriptorSize : kMaxModules;
        for (std::size_t i = 0; i < declared; ++i) {
            if (i >= kMaxModules) throw ParseError("too many delay-import descriptors");
            const std::uint64_t e = rva_to_offset(std::uint64_t{d.rva} + i * kDelayDescriptorSize);
            const auto attributes = r_.read<std::uint32_t>(e);
            const auto name = r_.read<std::uint32_t>(e + kDelayName);
            const auto iat = r_.read<std::uint32_t>(e + 12);
            const auto name_table = r_.read<std::uint32_t>(e + 16);
            if (!name && !iat && !name_table) break;

            const bool va_based = (attributes & kDelayRvaBased) == 0;
            ImportModule m;
            m.delay_loaded = true;
            m.descriptor_rva = static_cast<std::uint32_t>(d.rva + i * kDelayDescriptorSize);
            m.name_is_va = va_based;
            m.dll = r_.cstr(rva_to_offset(to_rva(name, va_based)));
            if (name_table) m.functions = read_thunks(to_rva(name_table, va_based), va_based);
            img_.imports.push_back(std::move(m));
        }
    }

    void parse_exports(DataDirectory d) {
        if (d.rva == 0) return;
        const std::uint64_t e = rva_to_offset(d.rva);
        const auto name_rva = r_.read<std::uint32_t>(e + kExportNameRva);
        const auto base = r_.read<std::uint32_t>(e + kExportOrdinalBase);
        const auto function_count = r_.read<std::uint32_t>(e + kExportFunctionCount);
        const auto name_count = r_.read<std::uint32_t>(e + kExportNameCount);
        const auto functions = r_.read<std::uint32_t>(e + kExportFunctions);
        const auto names = r_.read<std::uint32_t>(e + kExportNames);
        const auto name_ordinals = r_.read<std::uint32_t>(e + kExportNameOrdinals);
        if (function_count > kMaxExports || name_count > kMaxExports)
            throw ParseError("export table is implausibly large");

        if (name_rva) img_.export_name = r_.cstr(rva_to_offset(name_rva));

        std::vector<std::vector<std::string>> names_by_index(function_count);
        for (std::uint32_t j = 0; j < name_count; ++j) {
            const auto n = r_.read<std::uint32_t>(rva_to_offset(std::uint64_t{names} + j * 4ull));
            const auto index = r_.read<std::uint16_t>(rva_to_offset(std::uint64_t{name_ordinals} + j * 2ull));
            if (index < function_count) names_by_index[index].push_back(r_.cstr(rva_to_offset(n)));
        }

        for (std::uint32_t k = 0; k < function_count; ++k) {
            const auto rva = r_.read<std::uint32_t>(rva_to_offset(std::uint64_t{functions} + k * 4ull));
            if (rva == 0) continue;
            Export ex;
            ex.ordinal = base + k;
            ex.rva = rva;
            if (rva >= d.rva && std::uint64_t{rva} < std::uint64_t{d.rva} + d.size)
                ex.forwarder = r_.cstr(rva_to_offset(rva));
            if (names_by_index[k].empty()) {
                img_.exports.push_back(std::move(ex));
            } else {
                for (auto& n : names_by_index[k]) {
                    Export named = ex;
                    named.name = std::move(n);
                    img_.exports.push_back(std::move(named));
                }
            }
        }
    }

    Reader r_;
    Image img_;
};

}  // namespace

std::uint64_t Image::offset_of(std::uint64_t rva) const {
    if (rva > 0xffffffffull) throw ParseError("RVA overflow");
    for (const auto& s : sections) {
        const std::uint64_t extent = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.virtual_address && rva < std::uint64_t{s.virtual_address} + extent) {
            const std::uint64_t delta = rva - s.virtual_address;
            if (delta >= s.raw_size)
                throw ParseError("RVA " + hex(rva) + " points at zero-filled data in section " + s.name);
            return std::uint64_t{s.raw_offset} + delta;
        }
    }
    if (rva < size_of_headers) return rva;
    throw ParseError("RVA " + hex(rva) + " is outside every section");
}

Image parse(std::span<const std::uint8_t> data) { return Parser(data).run(); }

Image parse_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw ParseError("cannot open " + path.string());
    const std::streamoff size = in.tellg();
    if (size < 0) throw ParseError("cannot read " + path.string());
    if (size > 0xffffffffll) throw ParseError("file is larger than a PE file can be");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) throw ParseError("cannot read " + path.string());
    return parse(bytes);
}

}  // namespace g6shim::pe
