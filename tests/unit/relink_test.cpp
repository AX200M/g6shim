// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"
#include "sample_pe.hpp"

#include "g6shim/pe.hpp"
#include "g6shim/relink.hpp"

#include <random>

namespace {

using g6shim::relink::relink;
using g6shim::test::build_sample_pe;
using g6shim::test::kSampleDirectories;
using g6shim::test::kSampleHeaders;
using g6shim::test::kSampleOptional;
using g6shim::test::kSampleSections;
using g6shim::test::put;

constexpr std::size_t kSizeOfHeadersField = kSampleOptional + 60;
constexpr std::size_t kDllCharacteristicsField = kSampleOptional + 70;
constexpr std::size_t kFirstSectionVirtualSize = kSampleSections + 8;
constexpr std::size_t kFirstSectionCharacteristics = kSampleSections + 36;
constexpr std::size_t kAfterSectionTable = kSampleSections + 40;

template <class F>
bool throws_relink_error(F&& fn) {
    try {
        fn();
    } catch (const g6shim::relink::Error&) {
        return true;
    } catch (...) {
    }
    return false;
}

g6shim::relink::Options sample_renames() {
    g6shim::relink::Options opt;
    opt.renames["fakeconsole.dll"] = "g6shim_fakeconsole.dll";
    opt.renames["fakedelay.dll"] = "g6shim_fakedelay.dll";
    return opt;
}

std::vector<std::uint8_t> without_header_room() {
    auto tight = build_sample_pe();
    put<std::uint32_t>(tight, kSizeOfHeadersField, static_cast<std::uint32_t>(kAfterSectionTable));
    return tight;
}

}  // namespace

TEST(relink_renames_imports_into_a_new_section) {
    const auto original = build_sample_pe();
    const auto before = g6shim::pe::parse(original);

    auto opt = sample_renames();
    opt.renames["absent.dll"] = "x.dll";
    const auto r = relink(original, opt);
    CHECK(r.warnings.size() == 1);
    CHECK(g6shim::relink::default_new_name("CombASE.dll") == "g6shim_combase.dll");

    const auto img = g6shim::pe::parse(r.bytes);
    CHECK(img.warnings.empty());
    CHECK(img.subsystem == g6shim::pe::layout::kSubsystemWindowsGui);
    CHECK(img.sections.size() == 2);
    if (img.sections.size() == 2) {
        CHECK(img.sections[1].name == ".g6shim" && img.sections[1].virtual_address == 0x2000);
        CHECK(img.sections[1].raw_offset == original.size() && img.sections[1].raw_size == 0x200);
    }
    CHECK(img.size_of_image == 0x3000);
    CHECK(img.imports.size() == 3);
    if (img.imports.size() == 3) {
        CHECK(img.imports[0].dll == "g6shim_fakeconsole.dll" && img.imports[0].functions.size() == 2);
        CHECK(img.imports[0].functions.size() == 2 && img.imports[0].functions[0].name == "Foo");
        CHECK(img.imports[1].dll == "KERNEL32.dll");
        CHECK(img.imports[2].dll == "g6shim_fakedelay.dll" && img.imports[2].delay_loaded);
    }
    CHECK(img.exports.size() == before.exports.size() && img.entry_point_rva == before.entry_point_rva);

    g6shim::relink::Options again;
    again.renames["g6shim_fakeconsole.dll"] = "other.dll";
    const auto img2 = g6shim::pe::parse(relink(r.bytes, again).bytes);
    CHECK(img2.sections.size() == 3 && img2.imports.size() == 3 && img2.imports[0].dll == "other.dll");
    CHECK(original == build_sample_pe());

    g6shim::relink::Options none;
    none.renames["absent.dll"] = "x.dll";
    CHECK(throws_relink_error([&] { relink(r.bytes, none); }));
}

TEST(relink_padding_fallback) {
    const auto tight = without_header_room();
    CHECK(throws_relink_error([&] { relink(tight, sample_renames()); }));

    auto padded = tight;
    put<std::uint32_t>(padded, kFirstSectionVirtualSize, 0xF80);
    const auto r = relink(padded, sample_renames());
    const auto img = g6shim::pe::parse(r.bytes);
    CHECK(img.warnings.empty() && img.sections.size() == 1 && r.bytes.size() == padded.size());
    CHECK(img.sections.size() == 1 && img.sections[0].virtual_size > 0xF80 && img.sections[0].virtual_size <= 0x1000);
    CHECK(img.size_of_image == 0x2000);
    CHECK(img.imports.size() == 3 && img.imports[0].dll == "g6shim_fakeconsole.dll" &&
          img.imports[2].dll == "g6shim_fakedelay.dll");

    auto used = padded;
    used[kSampleHeaders + 0xF90] = 1;
    CHECK(throws_relink_error([&] { relink(used, sample_renames()); }));

    auto executable = padded;
    put<std::uint32_t>(executable, kFirstSectionCharacteristics, 0x60000020);
    const auto exec_img = g6shim::pe::parse(relink(executable, sample_renames()).bytes);
    CHECK(exec_img.imports.size() == 3 && exec_img.imports[0].dll == "g6shim_fakeconsole.dll");

    auto unreadable = padded;
    put<std::uint32_t>(unreadable, kFirstSectionCharacteristics, 0x00000040);
    CHECK(throws_relink_error([&] { relink(unreadable, sample_renames()); }));

    auto busy = build_sample_pe();
    busy[kAfterSectionTable + 5] = 1;
    put<std::uint32_t>(busy, kFirstSectionVirtualSize, 0x1000);
    CHECK(throws_relink_error([&] { relink(busy, sample_renames()); }));
}

TEST(relink_drops_signature_and_bound_imports) {
    const auto original = build_sample_pe();
    const auto before = g6shim::pe::parse(original);

    auto signed_pe = original;
    const auto cert = static_cast<std::uint32_t>(signed_pe.size());
    signed_pe.resize(cert + 16, 0xab);
    put<std::uint32_t>(signed_pe, kSampleDirectories + 4 * 8, cert);
    put<std::uint32_t>(signed_pe, kSampleDirectories + 4 * 8 + 4, 16);
    put<std::uint16_t>(signed_pe, kDllCharacteristicsField, 0x0080);
    const auto img3 = g6shim::pe::parse(relink(signed_pe, sample_renames()).bytes);
    CHECK(img3.directories.size() == 16 && img3.directories[4].rva == 0 && img3.directories[4].size == 0);
    CHECK((img3.dll_characteristics & 0x0080) == 0);
    CHECK(img3.sections.size() == 2 && img3.sections[1].raw_offset == cert);

    auto bound = original;
    put<std::uint32_t>(bound, kSampleDirectories + 11 * 8, 0x170);
    put<std::uint32_t>(bound, kSampleDirectories + 11 * 8 + 4, 0x20);
    for (std::size_t i = 0x170; i < 0x190; ++i) bound[i] = 0x11;
    const std::uint64_t timestamp = before.offset_of(before.directories[1].rva + 4);
    put<std::uint32_t>(bound, timestamp, 0xffffffff);
    const auto r4 = relink(bound, sample_renames());
    const auto img4 = g6shim::pe::parse(r4.bytes);
    CHECK(img4.directories.size() == 16 && img4.directories[11].rva == 0 && img4.sections.size() == 2);
    std::uint32_t ts = 1;
    std::memcpy(&ts, &r4.bytes[timestamp], 4);
    CHECK(ts == 0);
}

TEST(relink_corrupted_input_never_crashes) {
    const auto original = build_sample_pe();
    const auto opt = sample_renames();
    std::mt19937 rng(99);
    std::uniform_int_distribution<std::size_t> pos(0, 0x200 + 0x300);
    std::uniform_int_distribution<int> byte(0, 255), flips(1, 8);
    bool ok = true;
    for (int i = 0; i < 5000; ++i) {
        auto bytes = original;
        for (int f = flips(rng); f > 0; --f) bytes[pos(rng)] = static_cast<std::uint8_t>(byte(rng));
        try {
            const auto r = relink(bytes, opt);
            g6shim::pe::parse(r.bytes);
        } catch (const g6shim::pe::ParseError&) {
        } catch (const g6shim::relink::Error&) {
        } catch (...) {
            ok = false;
        }
    }
    CHECK(ok);
}
