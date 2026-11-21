// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "g6shim/relink.hpp"
#include "g6shim/pe.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace g6shim::relink {

namespace {

using namespace pe::layout;

constexpr std::uint16_t kForceIntegrity = 0x0080;
constexpr std::size_t kMaxSections = 96;
constexpr std::uint32_t kReadable = 0x40000000;
constexpr std::uint32_t kExecutable = 0x20000000;
constexpr std::uint32_t kDiscardable = 0x02000000;
constexpr std::uint32_t kInitializedReadOnlyData = 0x40000040;
constexpr std::uint32_t kMaxFileAlignment = 0x10000;
constexpr std::uint32_t kMaxSectionAlignment = 0x1000000;
constexpr std::uint64_t kPaddingAlignment = 16;
constexpr std::size_t kMaxNewName = 255;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

bool power_of_two(std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

class Bytes {
public:
    explicit Bytes(std::vector<std::uint8_t>& b) : b_(b) {}

    template <class T>
    T get(std::uint64_t off) const {
        check(off, sizeof(T));
        T v;
        std::memcpy(&v, b_.data() + off, sizeof v);
        return v;
    }
    template <class T>
    void put(std::uint64_t off, T v) {
        check(off, sizeof(T));
        std::memcpy(b_.data() + off, &v, sizeof v);
    }

private:
    void check(std::uint64_t off, std::size_t n) const {
        if (off > b_.size() || n > b_.size() - off) throw pe::ParseError("offset " + pe::hex(off) + " is past end of file");
    }
    std::vector<std::uint8_t>& b_;
};

std::uint64_t directory_entry(const pe::Image& img, std::uint32_t index) {
    return img.data_directory_offset + std::uint64_t{index} * kDirectorySize;
}

std::uint64_t section_header(const pe::Image& img, std::size_t index) {
    return img.section_table_offset + index * kSectionHeaderSize;
}

std::uint64_t first_free_header(const pe::Image& img) { return section_header(img, img.sections.size()); }

std::uint64_t header_room_limit(const pe::Image& img) {
    std::uint64_t limit = img.size_of_headers;
    for (const auto& s : img.sections)
        if (s.raw_size) limit = std::min<std::uint64_t>(limit, s.raw_offset);
    return limit;
}

void drop_signature(const pe::Image& img, Result& r) {
    Bytes b(r.bytes);
    if (img.directories.size() > kDirSecurity && img.directories[kDirSecurity].size) {
        const auto& d = img.directories[kDirSecurity];
        std::uint64_t data_end = img.size_of_headers;
        for (const auto& s : img.sections) data_end = std::max(data_end, std::uint64_t{s.raw_offset} + s.raw_size);
        if (d.rva >= data_end && std::uint64_t{d.rva} + d.size == r.bytes.size()) r.bytes.resize(d.rva);
        b.put<std::uint64_t>(directory_entry(img, kDirSecurity), 0);
        r.changes.push_back("removed the digital signature (patching invalidates it)");
    }
    if (img.dll_characteristics & kForceIntegrity) {
        b.put<std::uint16_t>(img.optional_header_offset + kOptDllCharacteristics,
                             static_cast<std::uint16_t>(img.dll_characteristics & ~kForceIntegrity));
        r.changes.push_back("cleared FORCE_INTEGRITY");
    }
}

void drop_bound_imports(const pe::Image& img, Result& r) {
    Bytes b(r.bytes);
    bool changed = false;
    if (img.directories.size() > kDirBoundImport && img.directories[kDirBoundImport].rva) {
        const auto& d = img.directories[kDirBoundImport];
        const std::uint64_t table_end = first_free_header(img);
        const std::uint64_t start = d.rva, end = start + d.size;
        if (start >= table_end && end <= img.size_of_headers && end <= r.bytes.size())
            std::fill(r.bytes.begin() + static_cast<std::ptrdiff_t>(start), r.bytes.begin() + static_cast<std::ptrdiff_t>(end),
                      std::uint8_t{0});
        b.put<std::uint64_t>(directory_entry(img, kDirBoundImport), 0);
        changed = true;
    }
    for (const auto& m : img.imports) {
        if (m.delay_loaded) continue;
        const std::uint64_t timestamp = img.offset_of(std::uint64_t{m.descriptor_rva} + kImportTimestamp);
        if (b.get<std::uint32_t>(timestamp) != 0) {
            b.put<std::uint32_t>(timestamp, 0);
            changed = true;
        }
    }
    if (changed) r.changes.push_back("removed bound imports");
}

std::uint32_t add_section(const pe::Image& img, Result& r, const std::vector<std::uint8_t>& data) {
    const std::uint32_t fa = img.file_alignment, sa = img.section_alignment;
    if (!power_of_two(fa) || !power_of_two(sa) || fa > kMaxFileAlignment || sa > kMaxSectionAlignment)
        throw Error("unusual file or section alignment");
    if (img.sections.size() >= kMaxSections) throw Error("too many sections to add another");

    const std::uint64_t header = first_free_header(img);
    if (header + kSectionHeaderSize > header_room_limit(img)) throw Error("no room for another section header");
    Bytes b(r.bytes);
    for (std::uint64_t i = header; i < header + kSectionHeaderSize; ++i)
        if (b.get<std::uint8_t>(i) != 0) throw Error("the space after the section table is in use");

    std::uint64_t end_va = img.size_of_image;
    for (const auto& s : img.sections)
        end_va = std::max(end_va, std::uint64_t{s.virtual_address} + std::max(s.virtual_size, s.raw_size));
    const std::uint64_t va = align_up(end_va, sa);
    const std::uint64_t image_size = align_up(va + data.size(), sa);
    if (image_size > 0xffffffffull) throw Error("image too large to add a section");

    const std::uint64_t raw = align_up(r.bytes.size(), fa);
    const std::uint64_t raw_size = align_up(data.size(), fa);
    if (raw + raw_size > 0xffffffffull) throw Error("file too large to add a section");
    r.bytes.resize(static_cast<std::size_t>(raw), 0);
    r.bytes.insert(r.bytes.end(), data.begin(), data.end());
    r.bytes.resize(static_cast<std::size_t>(raw + raw_size), 0);

    char name[kSectionNameSize] = {};
    std::memcpy(name, kSectionName, std::min<std::size_t>(sizeof name, std::strlen(kSectionName)));
    for (std::uint64_t j = 0; j < kSectionNameSize; ++j) b.put<char>(header + j, name[j]);
    b.put<std::uint32_t>(header + kSectionVirtualSize, static_cast<std::uint32_t>(data.size()));
    b.put<std::uint32_t>(header + kSectionVirtualAddress, static_cast<std::uint32_t>(va));
    b.put<std::uint32_t>(header + kSectionRawSize, static_cast<std::uint32_t>(raw_size));
    b.put<std::uint32_t>(header + kSectionRawOffset, static_cast<std::uint32_t>(raw));
    b.put<std::uint32_t>(header + kSectionCharacteristics, kInitializedReadOnlyData);

    const std::uint64_t coff = img.coff_header_offset, opt = img.optional_header_offset;
    b.put<std::uint16_t>(coff + kCoffSectionCount, static_cast<std::uint16_t>(img.sections.size() + 1));
    b.put<std::uint32_t>(opt + kOptSizeOfImage, static_cast<std::uint32_t>(image_size));
    const std::uint64_t initialized = std::uint64_t{b.get<std::uint32_t>(opt + kOptInitializedDataSize)} + raw_size;
    b.put<std::uint32_t>(opt + kOptInitializedDataSize,
                         static_cast<std::uint32_t>(std::min<std::uint64_t>(initialized, 0xffffffffull)));

    r.changes.push_back(std::string("added section ") + kSectionName + " (" + std::to_string(data.size()) + " bytes)");
    return static_cast<std::uint32_t>(va);
}

std::uint32_t use_section_padding(const pe::Image& img, Result& r, const std::vector<std::uint8_t>& data) {
    Bytes b(r.bytes);
    for (const bool data_only : {true, false}) {
        for (std::size_t i = 0; i < img.sections.size(); ++i) {
            const auto& s = img.sections[i];
            if (!(s.characteristics & kReadable) || s.virtual_size == 0) continue;
            if (data_only && (s.characteristics & (kExecutable | kDiscardable))) continue;
            const std::uint64_t start = align_up(s.virtual_size, kPaddingAlignment), end = start + data.size();
            if (end > s.raw_size || std::uint64_t{s.raw_offset} + s.raw_size > r.bytes.size()) continue;
            std::uint64_t next_va = img.size_of_image;
            for (const auto& o : img.sections)
                if (o.virtual_address > s.virtual_address) next_va = std::min<std::uint64_t>(next_va, o.virtual_address);
            if (std::uint64_t{s.virtual_address} + end > next_va) continue;
            bool unused = true;
            for (std::uint64_t k = s.raw_offset + start; k < s.raw_offset + end && unused; ++k)
                unused = b.get<std::uint8_t>(k) == 0;
            if (!unused) continue;

            std::copy(data.begin(), data.end(), r.bytes.begin() + static_cast<std::ptrdiff_t>(s.raw_offset + start));
            b.put<std::uint32_t>(section_header(img, i) + kSectionVirtualSize, static_cast<std::uint32_t>(end));
            r.changes.push_back("no room for a section header: put " + std::to_string(data.size()) +
                                " bytes in the padding of " + s.name);
            return static_cast<std::uint32_t>(s.virtual_address + start);
        }
    }
    throw Error("no room for another section header, and no section has enough unused padding");
}

std::uint32_t add_data(const pe::Image& img, Result& r, const std::vector<std::uint8_t>& data) {
    if (first_free_header(img) + kSectionHeaderSize > header_room_limit(img)) return use_section_padding(img, r, data);
    return add_section(img, r, data);
}

struct Repoint {
    std::uint64_t field;
    bool va;
    std::string old_name, new_name;
};

}  // namespace

std::string default_new_name(const std::string& dll) { return "g6shim_" + pe::to_lower_ascii(dll); }

Result relink(std::span<const std::uint8_t> input, const Options& options) {
    const pe::Image img = pe::parse(input);
    if (!img.warnings.empty()) throw Error("can't patch a damaged file: " + img.warnings.front());

    Result r;
    r.bytes.assign(input.begin(), input.end());

    std::vector<Repoint> repoints;
    std::set<std::string> matched;
    for (const auto& m : img.imports) {
        const auto it = options.renames.find(pe::to_lower_ascii(m.dll));
        if (it == options.renames.end()) continue;
        const std::string& to = it->second;
        if (to.empty() || to.size() > kMaxNewName || to.find('\0') != std::string::npos)
            throw Error("bad new name for " + m.dll);
        matched.insert(it->first);
        const std::uint64_t name_field = m.delay_loaded ? kDelayName : kImportName;
        repoints.push_back({img.offset_of(std::uint64_t{m.descriptor_rva} + name_field), m.name_is_va, m.dll, to});
    }
    for (const auto& [from, to] : options.renames)
        if (!matched.count(from)) r.warnings.push_back(from + " is not imported, so it was not renamed");

    const bool fix_subsystem = options.fix_subsystem && img.subsystem == kSubsystemXbox;
    if (repoints.empty() && !fix_subsystem) throw Error("nothing to change");

    drop_signature(img, r);
    if (fix_subsystem) {
        Bytes(r.bytes).put<std::uint16_t>(img.optional_header_offset + kOptSubsystem, kSubsystemWindowsGui);
        r.changes.push_back("subsystem: game console -> Windows GUI");
    }

    if (!repoints.empty()) {
        drop_bound_imports(img, r);

        std::vector<std::uint8_t> names;
        std::map<std::string, std::uint32_t> name_offset;
        for (const auto& rp : repoints) {
            if (name_offset.count(rp.new_name)) continue;
            name_offset[rp.new_name] = static_cast<std::uint32_t>(names.size());
            names.insert(names.end(), rp.new_name.begin(), rp.new_name.end());
            names.push_back(0);
            if (names.size() % 2) names.push_back(0);
        }
        const std::uint32_t va = add_data(img, r, names);

        std::set<std::string> reported;
        for (const auto& rp : repoints) {
            const std::uint64_t rva = std::uint64_t{va} + name_offset.at(rp.new_name);
            const std::uint64_t value = rp.va ? img.image_base + rva : rva;
            if (value > 0xffffffffull) throw Error("the new name's address doesn't fit in a legacy delay-import entry");
            Bytes(r.bytes).put<std::uint32_t>(rp.field, static_cast<std::uint32_t>(value));
            if (reported.insert(rp.old_name).second) r.changes.push_back("import " + rp.old_name + " -> " + rp.new_name);
        }
    }

    Bytes(r.bytes).put<std::uint32_t>(img.optional_header_offset + kOptCheckSum, 0);
    return r;
}

}  // namespace g6shim::relink
