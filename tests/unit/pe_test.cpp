// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"
#include "sample_pe.hpp"

#include "g6shim/pe.hpp"

#include <random>
#include <span>

namespace {

using g6shim::pe::ParseError;
using g6shim::test::build_sample_pe;

template <class F>
bool only_parse_errors(F&& fn) {
    try {
        fn();
    } catch (const ParseError&) {
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

TEST(pe_reads_sample) {
    const auto bytes = build_sample_pe();
    const auto img = g6shim::pe::parse(bytes);
    CHECK(img.machine == 0x8664);
    CHECK(img.pe32_plus);
    CHECK(img.is_dll());
    CHECK(img.subsystem == g6shim::pe::layout::kSubsystemXbox);
    CHECK(g6shim::pe::subsystem_name(img.subsystem) == "game console");
    CHECK(img.entry_point_rva == 0x1f00);
    CHECK(img.image_base == 0x180000000ull);
    CHECK(img.sections.size() == 1 && img.sections[0].name == ".rdata");
    CHECK(img.warnings.empty());

    CHECK(img.imports.size() == 3);
    if (img.imports.size() == 3) {
        const auto& fake = img.imports[0];
        CHECK(fake.dll == "fakeconsole.dll" && !fake.delay_loaded);
        CHECK(fake.functions.size() == 2);
        if (fake.functions.size() == 2) {
            CHECK(fake.functions[0].name == "Foo" && fake.functions[0].hint == 1 && !fake.functions[0].ordinal);
            CHECK(fake.functions[1].ordinal == 7 && fake.functions[1].display() == "#7");
        }
        CHECK(img.imports[1].dll == "KERNEL32.dll");
        CHECK(img.imports[1].functions.size() == 1 && img.imports[1].functions[0].name == "ExitProcess");
        CHECK(img.imports[2].dll == "fakedelay.dll" && img.imports[2].delay_loaded);
        CHECK(img.imports[2].functions.size() == 1 && img.imports[2].functions[0].name == "DelayFn");
    }

    CHECK(img.export_name == "test.dll");
    CHECK(img.exports.size() == 3);
    bool alpha = false, unnamed = false, gamma = false;
    for (const auto& e : img.exports) {
        if (e.name == "Alpha") alpha = e.ordinal == 1 && e.rva == 0x1f00 && e.forwarder.empty();
        if (e.name.empty()) unnamed = e.ordinal == 2 && e.rva == 0x1f10;
        if (e.name == "Gamma") gamma = e.ordinal == 3 && e.forwarder == "other.Beta";
    }
    CHECK(alpha && unnamed && gamma);
}

TEST(pe_records_header_layout) {
    const auto img = g6shim::pe::parse(build_sample_pe());
    CHECK(img.coff_header_offset == g6shim::test::kSampleCoff);
    CHECK(img.optional_header_offset == g6shim::test::kSampleOptional);
    CHECK(img.section_table_offset == g6shim::test::kSampleSections);
    CHECK(img.data_directory_offset == g6shim::test::kSampleDirectories);
    CHECK(img.section_alignment == 0x1000 && img.file_alignment == 0x200);
    CHECK(img.size_of_image == 0x2000 && img.size_of_headers == 0x200);
    CHECK(img.directories.size() == 16);
    if (img.directories.size() == 16 && img.imports.size() == 3) {
        CHECK(img.imports[0].descriptor_rva == img.directories[1].rva);
        CHECK(img.imports[1].descriptor_rva == img.directories[1].rva + 20);
        CHECK(img.imports[2].descriptor_rva == img.directories[13].rva && !img.imports[2].name_is_va);
    }
    CHECK(img.offset_of(0x1000) == 0x200 && img.offset_of(0x1234) == 0x434 && img.offset_of(0x10) == 0x10);
    bool threw = false;
    try {
        img.offset_of(0x5000);
    } catch (const ParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(pe_truncated_files_raise_parse_errors) {
    const auto bytes = build_sample_pe();
    bool ok = true;
    for (std::size_t n = 0; n < bytes.size(); ++n)
        ok &= only_parse_errors([&] { g6shim::pe::parse(std::span(bytes.data(), n)); });
    CHECK(ok);
}

TEST(pe_corrupted_files_raise_parse_errors) {
    const auto original = build_sample_pe();
    std::mt19937 rng(1234);
    std::uniform_int_distribution<std::size_t> pos(0, 0x200 + 0x300);
    std::uniform_int_distribution<int> byte(0, 255), flips(1, 8);
    bool ok = true;
    for (int i = 0; i < 20000; ++i) {
        auto bytes = original;
        for (int f = flips(rng); f > 0; --f) bytes[pos(rng)] = static_cast<std::uint8_t>(byte(rng));
        ok &= only_parse_errors([&] { g6shim::pe::parse(bytes); });
    }
    CHECK(ok);
}

TEST(pe_rejects_non_pe) {
    std::vector<std::uint8_t> junk(1000, 0x41);
    bool threw = false;
    try {
        g6shim::pe::parse(junk);
    } catch (const ParseError&) {
        threw = true;
    }
    CHECK(threw);
}
