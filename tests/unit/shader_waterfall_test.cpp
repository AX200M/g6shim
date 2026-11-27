// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "g6shim/shader/emit/lower.hpp"
#include "g6shim/shader/error.hpp"
#include "g6shim/shader/io.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"
#include "g6shim/shader/structure.hpp"

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;

constexpr std::uint32_t kSEndPgm = 0xBF81'0000u;

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> code;
    code.reserve(words.size() * 4);
    for (std::uint32_t word : words) {
        code.push_back(static_cast<std::uint8_t>(word));
        code.push_back(static_cast<std::uint8_t>(word >> 8));
        code.push_back(static_cast<std::uint8_t>(word >> 16));
        code.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return code;
}

RootParam param(RootType::Kind kind, std::uint8_t user_data, std::uint32_t sgprs) {
    RootParam entry;
    entry.root = RootType{kind, 0};
    entry.user_data = user_data;
    entry.sgprs = sgprs;
    entry.base_register = 0;
    entry.table_offset_dwords = 0;
    entry.slot = user_data;
    return entry;
}

std::optional<Structured> lanes_of(const Program& p) {
    try {
        return structure(p);
    } catch (const ParseError&) {
        return std::nullopt;
    }
}

std::string reason_of(const ParseError& error) {
    const std::string message = error.what();
    const std::size_t start = message.find(": ");
    const std::size_t end = message.rfind(" is not supported");
    if (start == std::string::npos || end == std::string::npos || end <= start) return message;
    return message.substr(start + 2, end - start - 2);
}

Resources waterfall(const std::vector<std::uint32_t>& words, bool wave64) {
    const Program p = build(bytes_of(words), wave64, 256);
    const std::vector<RootParam> layout = {param(RootType::Kind::Table, 0, 1),
                                           param(RootType::Kind::Table, 1, 1)};
    const std::optional<Structured> lanes = lanes_of(p);
    return track(p, layout, HwStage::Cs, lanes ? &*lanes : nullptr);
}

std::string refusal(const std::vector<std::uint32_t>& words, bool wave64) {
    try {
        (void)waterfall(words, wave64);
    } catch (const ParseError& error) {
        return reason_of(error);
    }
    return "<not refused>";
}

std::size_t word_at(const std::vector<std::uint32_t>& words, std::uint32_t word) {
    const auto found = std::find(words.begin(), words.end(), word);
    return found == words.end() ? words.size() : static_cast<std::size_t>(found - words.begin());
}

std::size_t block_at(const std::vector<std::uint32_t>& words, std::size_t offset) {
    const Program p = build(bytes_of(words), false, 256);
    for (std::size_t b = 0; b < p.blocks.size(); ++b)
        if (p.blocks[b].offset == offset) return b;
    return p.blocks.size();
}

std::uint32_t table_offset(const Resources& r, const Source& source) {
    if (source.kind == Source::Kind::Loaded && source.first == 0) {
        for (const auto& entry : r.scalar)
            if (entry.first == source.load) return entry.second.offset;
    }
    return UINT32_MAX;
}

const std::vector<std::uint32_t> kWaterfallFetchedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0x3402'0085, 0xDC38'8000, 0x0404'0001,
    0xDC38'8010, 0x0804'0001, 0xF408'0203, 0xFA00'0000, 0x7E04'0D00, 0x7E06'0302, 0xBF8C'0070,
    0xBE94'037E, 0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0x7E20'0508, 0x7E22'0509,
    0x7E24'050A, 0x7E26'050B, 0xD4E2'0015, 0x0002'080C, 0xD4E2'0016, 0x0002'0C0E, 0x8715'1615,
    0xD4E2'0016, 0x0002'1010, 0x8715'1615, 0xD4E2'0016, 0x0002'1412, 0x8715'1615, 0xBE95'3C15,
    0xBF80'0000, 0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70, 0x897E'157E, 0xBF89'FFE6, 0xBEFE'0314,
    0xD834'0000, 0x0000'0C00, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallFetchedWave64 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0x3402'0085, 0xDC38'8000, 0x0404'0001,
    0xDC38'8010, 0x0804'0001, 0xF408'0203, 0xFA00'0000, 0x7E04'0D00, 0x7E06'0302, 0xBF8C'0070,
    0xBE94'047E, 0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0x7E20'0508, 0x7E22'0509,
    0x7E24'050A, 0x7E26'050B, 0xD4E2'0016, 0x0002'080C, 0xD4E2'0018, 0x0002'0C0E, 0x8796'1816,
    0xD4E2'0018, 0x0002'1010, 0x8796'1816, 0xD4E2'0018, 0x0002'1412, 0x8796'1816, 0xBE96'2416,
    0xBF80'0000, 0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70, 0x89FE'167E, 0xBF89'FFE6, 0xBEFE'0414,
    0xD834'0000, 0x0000'0C00, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallPickedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF40C'0602, 0xFA00'0000, 0xF40C'0802,
    0xFA00'0020, 0xF408'0203, 0xFA00'0000, 0x7E04'0D00, 0x7E06'0302, 0x7D88'0090, 0xBF8C'C07F,
    0x7E08'0218, 0x0208'0820, 0x7E0A'0219, 0x020A'0A21, 0x7E0C'021A, 0x020C'0C22, 0x7E0E'021B,
    0x020E'0E23, 0x7E10'021C, 0x0210'1024, 0x7E12'021D, 0x0212'1225, 0x7E14'021E, 0x0214'1426,
    0x7E16'021F, 0x0216'1627, 0xBE94'037E, 0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507,
    0x7E20'0508, 0x7E22'0509, 0x7E24'050A, 0x7E26'050B, 0xD4E2'0015, 0x0002'080C, 0xD4E2'0016,
    0x0002'0C0E, 0x8715'1615, 0xD4E2'0016, 0x0002'1010, 0x8715'1615, 0xD4E2'0016, 0x0002'1412,
    0x8715'1615, 0xBE95'3C15, 0xBF80'0000, 0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70, 0x897E'157E,
    0xBF89'FFE6, 0xBEFE'0314, 0xD834'0000, 0x0000'0C00, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallJoinedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF40C'0602, 0xFA00'0000, 0xF40C'0802,
    0xFA00'0020, 0xF408'0203, 0xFA00'0000, 0xF408'0A02, 0xFA00'0040, 0x7E04'0D00, 0x7E06'0302,
    0x3420'0082, 0x7D88'0090, 0xBF8C'C07F, 0xBE94'3C6A, 0x8914'147E, 0xBF88'0008, 0x7E08'0218,
    0x7E0A'0219, 0x7E0C'021A, 0x7E0E'021B, 0x7E10'021C, 0x7E12'021D, 0x7E14'021E, 0x7E16'021F,
    0xBE94'3D14, 0x897E'147E, 0xBF88'0008, 0x7E08'0220, 0x7E0A'0221, 0x7E0C'0222, 0x7E0E'0223,
    0x7E10'0224, 0x7E12'0225, 0x7E14'0226, 0x7E16'0227, 0x887E'147E, 0xBE97'037E, 0x7E18'0504,
    0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0x7E20'0508, 0x7E22'0509, 0x7E24'050A, 0x7E26'050B,
    0xD4E2'0015, 0x0002'080C, 0xD4E2'0016, 0x0002'0C0E, 0x8715'1615, 0xD4E2'0016, 0x0002'1010,
    0x8715'1615, 0xD4E2'0016, 0x0002'1412, 0x8715'1615, 0xBE95'3C15, 0xBF80'0000, 0xF09C'0F08,
    0x0043'0C02, 0xBF8C'3F70, 0x897E'157E, 0xBF89'FFE6, 0xBEFE'0317, 0xE070'1000, 0x800A'0C10,
    kSEndPgm};

const std::vector<std::uint32_t> kWaterfallIndexedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF408'0203, 0xFA00'0000, 0xF408'0A02,
    0xFA00'0040, 0x7E04'0D00, 0x7E06'0302, 0x3420'0082, 0x360E'0081, 0x340E'0E85, 0xBF8C'C07F,
    0xBE97'037E, 0xBE95'037E, 0x7E28'0507, 0x7DA4'0E14, 0xF40C'0302, 0x2800'0000, 0xBF8C'C07F,
    0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70, 0xBE95'4715, 0xBF89'FFF6, 0xBEFE'0317, 0xE070'1000,
    0x800A'0C10, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallJoinedMixedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF40C'0602, 0xFA00'0000, 0xF40C'0802,
    0xFA00'0020, 0xF408'0203, 0xFA00'0000, 0xF408'0A02, 0xFA00'0040, 0x7E04'0D00, 0x7E06'0302,
    0x3420'0082, 0x3602'0081, 0x3402'0285, 0x7D88'0090, 0xBF8C'C07F, 0xBE94'3C6A, 0x8914'147E,
    0xBF88'0004, 0xDC38'8000, 0x0404'0001, 0xDC38'8010, 0x0804'0001, 0xBE94'3D14, 0x897E'147E,
    0xBF88'0011, 0x7D88'0098, 0x7E08'0218, 0x0208'0820, 0x7E0A'0219, 0x020A'0A21, 0x7E0C'021A,
    0x020C'0C22, 0x7E0E'021B, 0x020E'0E23, 0x7E10'021C, 0x0210'1024, 0x7E12'021D, 0x0212'1225,
    0x7E14'021E, 0x0214'1426, 0x7E16'021F, 0x0216'1627, 0x887E'147E, 0xBE97'037E, 0xBF8C'3F70,
    0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0x7E20'0508, 0x7E22'0509, 0x7E24'050A,
    0x7E26'050B, 0xD4E2'0015, 0x0002'080C, 0xD4E2'0016, 0x0002'0C0E, 0x8715'1615, 0xD4E2'0016,
    0x0002'1010, 0x8715'1615, 0xD4E2'0016, 0x0002'1412, 0x8715'1615, 0xBE95'3C15, 0xBF80'0000,
    0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70, 0x897E'157E, 0xBF89'FFE6, 0xBEFE'0317, 0xE070'1000,
    0x800A'0C10, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallLoopedWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF40C'0602, 0xFA00'0000, 0xF40C'0802,
    0xFA00'0020, 0xF408'0203, 0xFA00'0000, 0xF408'0A02, 0xFA00'0040, 0x7E04'0D00, 0x7E06'0302,
    0x3420'0082, 0x7E08'0218, 0x7E0A'0219, 0x7E0C'021A, 0x7E0E'021B, 0x7E10'021C, 0x7E12'021D,
    0x7E14'021E, 0x7E16'021F, 0xBF8C'C07F, 0xBE97'037E, 0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506,
    0x7E1E'0507, 0x7E20'0508, 0x7E22'0509, 0x7E24'050A, 0x7E26'050B, 0xD4E2'0015, 0x0002'080C,
    0xD4E2'0016, 0x0002'0C0E, 0x8715'1615, 0xD4E2'0016, 0x0002'1010, 0x8715'1615, 0xD4E2'0016,
    0x0002'1412, 0x8715'1615, 0xBE95'3C15, 0xBF80'0000, 0xF09C'0F08, 0x0043'0C02, 0xBF8C'3F70,
    0x897E'157E, 0x7E08'0220, 0x7E0A'0221, 0x7E0C'0222, 0x7E0E'0223, 0x7E10'0224, 0x7E12'0225,
    0x7E14'0226, 0x7E16'0227, 0xBF89'FFDE, 0xBEFE'0317, 0xE070'1000, 0x800A'0C10, kSEndPgm};

const std::vector<std::uint32_t> kWaterfallJoinedBufferWave32 = {
    0xBE84'0300, 0xBE85'0380, 0xF408'0602, 0xFA00'0000, 0xF408'0802, 0xFA00'0020, 0xF408'0A02,
    0xFA00'0040, 0x3420'0082, 0xD4C4'001E, 0x0002'0098, 0x7D88'0090, 0xBF8C'C07F, 0xBE94'3C6A,
    0x8914'147E, 0xBF88'0004, 0x7E08'0218, 0x7E0A'0219, 0x7E0C'021A, 0x7E0E'021B, 0xBE94'3D14,
    0x897E'147E, 0xBF88'0007, 0xBE9F'3C1E, 0xBF88'0004, 0x7E08'0220, 0x7E0A'0221, 0x7E0C'0222,
    0x7E0E'0223, 0x887E'1F7E, 0x887E'147E, 0xBE97'3C1E, 0xBF88'0013, 0xBE9D'037E, 0x7E18'0504,
    0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0xD4E2'0015, 0x0002'080C, 0xD4E2'0016, 0x0002'0C0E,
    0x8715'1615, 0xBE95'3C15, 0xE030'1000, 0x8003'0C10, 0xBF8C'3F70, 0x897E'157E, 0xBF89'FFF1,
    0xBEFE'031D, 0xE070'1000, 0x800A'0C10, 0x887E'177E, kSEndPgm};

std::vector<std::uint32_t> cmpx_waterfall(bool wave64, bool folded) {
    const std::vector<std::uint32_t>& base = wave64 ? kWaterfallFetchedWave64 : kWaterfallJoinedWave32;
    const std::uint32_t todo = wave64 ? 0xBE94'047E : 0xBE97'037E;
    const std::uint32_t restore = wave64 ? 0xBEFE'0414 : 0xBEFE'0317;
    const std::size_t reads = word_at(base, 0x7E18'0504);
    const std::size_t sample = word_at(base, 0xF09C'0F08);
    std::vector<std::uint32_t> w(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(word_at(base, todo) + 1));
    w.push_back(wave64 ? 0xBE96'047E : 0xBE95'037E);
    w.insert(w.end(), base.begin() + static_cast<std::ptrdiff_t>(reads),
             base.begin() + static_cast<std::ptrdiff_t>(reads + 8));
    w.insert(w.end(), {0x7DE4'080C, 0x7DE4'0C0E, 0x7DE4'1010, 0x7DE4'1412});
    w.insert(w.end(), base.begin() + static_cast<std::ptrdiff_t>(sample),
             base.begin() + static_cast<std::ptrdiff_t>(sample + 2));
    if (!wave64 && folded) {
        w.insert(w.end(), {0xBE95'4715, 0xBF89'FFF0});
    } else if (!wave64) {
        w.insert(w.end(), {0x8A15'7E15, 0xBEFE'0315, 0xBF89'FFEF});
    } else if (folded) {
        w.insert(w.end(), {0xBE96'3A16, 0xBF89'FFF0});
    } else {
        w.insert(w.end(), {0x8A96'7E16, 0xBEFE'0416, 0xBF89'FFEF});
    }
    w.insert(w.end(), base.begin() + static_cast<std::ptrdiff_t>(word_at(base, restore)), base.end());
    return w;
}

const Descriptor* first_resource(const Resources& r) {
    for (const Descriptor& descriptor : r.descriptors)
        if (descriptor.role == Role::Resource) return &descriptor;
    return nullptr;
}

const Descriptor* first_lane_phi(const Resources& r) {
    for (const Descriptor& descriptor : r.descriptors)
        if (descriptor.source.kind == Source::Kind::LanePhi) return &descriptor;
    return nullptr;
}

TEST(waterfall_the_console_waterfall_over_a_first_lane_offset_is_one) {
    const Resources r = waterfall(kWaterfallIndexedWave32, false);
    const Descriptor* image = first_resource(r);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    CHECK(image->source.kind == Source::Kind::Loaded);
    CHECK(image->source.first == 0);
    const Site load = image->source.load;
    bool dynamic = false;
    for (const auto& entry : r.scalar)
        if (entry.first == load) dynamic = entry.second.dynamic.has_value();
    CHECK(dynamic);
    CHECK(r.waterfall.compares.size() == 1);
    CHECK(r.waterfall.reads.size() == 1);
    CHECK(r.waterfall.loads == std::vector<Site>{load});
}

TEST(waterfall_descriptors_read_from_the_first_lane_of_per_lane_loads_are_traced_to_them) {
    const std::array<std::pair<const std::vector<std::uint32_t>*, bool>, 2> cases = {
        std::pair<const std::vector<std::uint32_t>*, bool>{&kWaterfallFetchedWave32, false},
        std::pair<const std::vector<std::uint32_t>*, bool>{&kWaterfallFetchedWave64, true}};
    for (const auto& [words, wave64] : cases) {
        const Resources r = waterfall(*words, wave64);
        const Descriptor* image = first_resource(r);
        CHECK(image != nullptr);
        if (image == nullptr) continue;
        CHECK(image->source.kind == Source::Kind::Fetched);
        CHECK(image->source.first == 0);
        const Site load = image->source.load;
        bool found = false;
        for (const auto& entry : r.scalar)
            if (entry.first == load) {
                found = true;
                CHECK(entry.second.param == 0);
                CHECK(entry.second.offset == 0);
                CHECK(entry.second.dynamic.has_value());
            }
        CHECK(found);
        bool sampler = false;
        for (const Descriptor& descriptor : r.descriptors)
            if (descriptor.role == Role::Sampler)
                sampler = descriptor.source.kind == Source::Kind::Loaded && descriptor.source.first == 0;
        CHECK(sampler);
        CHECK(r.waterfall.compares.size() == 4);
        CHECK(r.waterfall.reads.size() == 8);
    }
}

TEST(waterfall_descriptors_read_from_the_first_lane_of_a_per_lane_choice_are_traced_per_lane) {
    const Resources r = waterfall(kWaterfallPickedWave32, false);
    CHECK(!r.descriptors.empty());
    if (r.descriptors.empty()) return;
    const Source& image = r.descriptors[0].source;
    CHECK(image.kind == Source::Kind::Lanes);
    std::vector<std::uint32_t> offsets;
    for (const Source& arg : image.args) offsets.push_back(table_offset(r, arg));
    CHECK(offsets == (std::vector<std::uint32_t>{0, 0x20}));
    CHECK(r.waterfall.compares.size() == 4);
}

TEST(waterfall_a_first_lane_read_outside_a_loop_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallFetchedWave32;
    const std::size_t back = word_at(words, 0xBF89'FFE6);
    CHECK(back < words.size());
    if (back < words.size()) words[back] = 0xBF80'0000;
    CHECK(refusal(words, false) == "a descriptor read from the first lane outside a waterfall loop");
}

TEST(waterfall_a_first_lane_read_without_its_compare_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallFetchedWave32;
    const std::size_t cmp = word_at(words, 0x0002'080C);
    CHECK(cmp >= 1 && cmp < words.size());
    if (cmp < 1 || cmp >= words.size()) return;
    words[cmp - 1] = 0xBF80'0000;
    words[cmp] = 0xBF80'0000;
    CHECK(refusal(words, false) == "a waterfall loop whose compares do not select the access's lanes");
}

TEST(waterfall_a_compare_that_does_not_mask_the_access_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    const std::size_t save = word_at(words, 0xBE95'3C15);
    CHECK(save < words.size());
    if (save < words.size()) words[save] = 0xBE95'037E;
    CHECK(refusal(words, false) == "a waterfall loop whose compares do not select the access's lanes");
}

TEST(waterfall_an_access_under_a_further_mask_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    const std::size_t nop = word_at(words, 0xBE95'3C15) + 1;
    CHECK(nop < words.size());
    if (nop >= words.size()) return;
    CHECK(words[nop] == 0xBF80'0000);
    words[nop] = 0x877E'6A7E;
    CHECK(refusal(words, false) == "a waterfall loop whose compares do not select the access's lanes");
}

TEST(waterfall_a_loop_that_drops_unmatched_lanes_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    const std::size_t retry = word_at(words, 0x897E'157E);
    CHECK(retry < words.size());
    if (retry < words.size()) words[retry] = 0xBEFE'0380;
    CHECK(refusal(words, false) == "a waterfall loop whose retry does not keep the lanes still waiting");
}

TEST(waterfall_a_retry_that_keeps_other_lanes_than_those_waiting_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    const std::size_t retry = word_at(words, 0x897E'157E);
    CHECK(retry < words.size());
    if (retry < words.size()) words[retry] = 0x8A7E'7E6A;
    CHECK(refusal(words, false) == "a waterfall loop whose retry does not keep the lanes still waiting");
}

TEST(waterfall_a_wave64_retry_of_one_half_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallFetchedWave64;
    const std::size_t retry = word_at(words, 0x89FE'167E);
    CHECK(retry < words.size());
    if (retry < words.size()) words[retry] = 0x897E'167E;
    CHECK(refusal(words, true) == "a waterfall loop whose retry does not keep the lanes still waiting");
}

TEST(waterfall_a_descriptor_used_after_exec_is_restored_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    const std::size_t sample = word_at(words, 0xF09C'0F08);
    const std::size_t restore = word_at(words, 0xBEFE'0317);
    CHECK(sample + 1 < words.size());
    CHECK(restore < words.size());
    if (sample + 1 >= words.size() || restore >= words.size()) return;
    const std::array<std::uint32_t, 2> image = {words[sample], words[sample + 1]};
    words[sample] = 0xBF80'0000;
    words[sample + 1] = 0xBF80'0000;
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(restore + 1), image.begin(), image.end());
    CHECK(refusal(words, false) == "a descriptor read from the first lane outside a waterfall loop");
}

TEST(waterfall_a_wave64_access_with_exec_hi_overwritten_is_no_waterfall) {
    std::vector<std::uint32_t> words = kWaterfallFetchedWave64;
    const std::size_t save = word_at(words, 0xBE96'2416);
    CHECK(save + 1 < words.size());
    if (save + 1 < words.size()) words[save + 1] = 0xBEFF'03C1;
    CHECK(refusal(words, true) == "a waterfall loop whose compares do not select the access's lanes");
}

TEST(waterfall_a_waterfall_narrowed_by_v_cmpx_is_one) {
    for (bool wave64 : {false, true}) {
        for (bool folded : {false, true}) {
            const Resources r = waterfall(cmpx_waterfall(wave64, folded), wave64);
            CHECK(r.waterfall.compares.size() == 4);
            CHECK(r.waterfall.reads.size() == 8);
        }
    }
}

TEST(waterfall_a_v_cmpx_waterfall_whose_retry_drops_lanes_is_none) {
    std::vector<std::uint32_t> words = cmpx_waterfall(false, false);
    const std::size_t retry = word_at(words, 0x8A15'7E15);
    CHECK(retry < words.size());
    if (retry < words.size()) words[retry] = 0x8A15'7E6A;
    CHECK(refusal(words, false) == "a waterfall loop whose retry does not keep the lanes still waiting");
}

TEST(waterfall_a_waterfall_nested_in_another_is_refused_by_name) {
    const std::vector<std::uint32_t> words = {
        0xBE84'0300, 0xBE85'0380, 0xBE86'0301, 0xBE87'0380, 0xF408'0A02, 0xFA00'0040, 0x7E04'0D00,
        0x7E06'0302, 0x3420'0082, 0x3602'0081, 0x3402'0285, 0x3622'0080, 0x3422'2284, 0xDC38'8000,
        0x0404'0001, 0xDC38'8010, 0x0804'0001, 0xDC38'8000, 0x1406'0011, 0xBF8C'0070, 0xBE94'037E,
        0x7E18'0504, 0x7E1A'0505, 0x7E1C'0506, 0x7E1E'0507, 0x7E20'0508, 0x7E22'0509, 0x7E24'050A,
        0x7E26'050B, 0xD4E2'0015, 0x0002'080C, 0xD4E2'0016, 0x0002'0C0E, 0x8715'1615, 0xD4E2'0016,
        0x0002'1010, 0x8715'1615, 0xD4E2'0016, 0x0002'1412, 0x8715'1615, 0xBE95'3C15, 0xBE9A'037E,
        0x7E10'0514, 0x7E12'0515, 0x7E14'0516, 0x7E16'0517, 0xD4E2'0018, 0x0002'2808, 0xD4E2'0019,
        0x0002'2C0A, 0x8718'1918, 0xBE98'3C18, 0xF09C'0F08, 0x0043'0C02, 0x897E'187E, 0xBF89'FFF2,
        0xBEFE'031A, 0x897E'157E, 0xBF89'FFDA, 0xBEFE'0314, 0xE070'1000, 0x800A'0C10, kSEndPgm};
    CHECK(refusal(words, false) == "a waterfall loop nested in another or split over blocks");
}

TEST(waterfall_descriptors_read_from_the_first_lane_of_lds_fail_naming_it) {
    std::vector<std::uint32_t> words = kWaterfallFetchedWave32;
    CHECK(words.size() > 9);
    if (words.size() <= 9) return;
    words[5] = 0xDBFC'0000;
    words[6] = 0x0400'0001;
    words[7] = 0xDBFC'0010;
    words[8] = 0x0800'0001;
    CHECK(refusal(words, false) == "a descriptor read from the first lane of an LDS read");
}

TEST(waterfall_descriptors_read_from_the_first_lane_of_an_if_else_join_are_traced_per_path) {
    const Resources r = waterfall(kWaterfallJoinedWave32, false);
    const Descriptor* image = first_lane_phi(r);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    const Source& outer = image->source;
    CHECK(outer.block == block_at(kWaterfallJoinedWave32, 0x9c));
    CHECK(outer.options.size() == 2);
    if (outer.options.size() != 2) return;
    CHECK(outer.options[0].has_value());
    CHECK(outer.options[1].has_value());
    if (!outer.options[0] || !outer.options[1]) return;
    CHECK(table_offset(r, *outer.options[1]) == 0x20);
    const Source& flow = *outer.options[0];
    CHECK(flow.kind == Source::Kind::LanePhi);
    CHECK(flow.block == block_at(kWaterfallJoinedWave32, 0x70));
    CHECK(flow.options.size() == 2);
    if (flow.options.size() != 2) return;
    CHECK(!flow.options[0].has_value());
    CHECK(flow.options[1].has_value());
    if (!flow.options[1]) return;
    CHECK(table_offset(r, *flow.options[1]) == 0);
    CHECK(r.waterfall.compares.size() == 4);
    CHECK(r.waterfall.reads.size() == 8);
    const Program p = build(bytes_of(kWaterfallJoinedWave32), false, 256);
    const std::vector<RootParam> layout = {param(RootType::Kind::Table, 0, 1),
                                           param(RootType::Kind::Table, 1, 1)};
    std::string reason = "<not refused>";
    try {
        (void)track(p, layout, HwStage::Cs, nullptr);
    } catch (const ParseError& error) {
        reason = reason_of(error);
    }
    CHECK(reason == "a descriptor read from the first lane of a phi, without the lanes' paths");
}

TEST(waterfall_descriptors_joined_from_a_per_lane_load_and_a_per_lane_choice_are_traced_per_path) {
    const Resources r = waterfall(kWaterfallJoinedMixedWave32, false);
    const Descriptor* image = first_lane_phi(r);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    const Source& outer = image->source;
    CHECK(outer.block == block_at(kWaterfallJoinedMixedWave32, 0xb8));
    CHECK(outer.options.size() == 2);
    if (outer.options.size() != 2) return;
    CHECK(outer.options[0].has_value());
    CHECK(outer.options[1].has_value());
    if (!outer.options[0] || !outer.options[1]) return;
    const Source& flow = *outer.options[0];
    const Source& picked = *outer.options[1];
    CHECK(flow.kind == Source::Kind::LanePhi);
    CHECK(picked.kind == Source::Kind::Lanes);
    CHECK(flow.options.size() == 2);
    if (flow.options.size() != 2) return;
    CHECK(!flow.options[0].has_value());
    CHECK(flow.options[1].has_value());
    if (!flow.options[1]) return;
    CHECK(flow.options[1]->kind == Source::Kind::Fetched);
    CHECK(flow.options[1]->first == 0);
    bool found = false;
    for (const auto& entry : r.scalar)
        if (entry.first == flow.options[1]->load) {
            found = true;
            CHECK(entry.second.param == 0);
            CHECK(entry.second.offset == 0);
            CHECK(entry.second.dynamic.has_value());
        }
    CHECK(found);
    std::vector<std::uint32_t> offsets;
    for (const Source& arg : picked.args) offsets.push_back(table_offset(r, arg));
    CHECK(offsets == (std::vector<std::uint32_t>{0, 0x20}));
    CHECK(r.waterfall.compares.size() == 4);
}

TEST(waterfall_a_descriptor_read_from_the_first_lane_of_a_loop_phi_fails_naming_it) {
    CHECK(refusal(kWaterfallLoopedWave32, false) ==
          "a descriptor read from the first lane of a phi at a loop header");
}

TEST(waterfall_a_per_lane_descriptor_some_lanes_do_not_write_fails) {
    std::vector<std::uint32_t> words = kWaterfallJoinedWave32;
    std::size_t skip = words.size();
    for (std::size_t i = words.size(); i > 0; --i)
        if (words[i - 1] == 0xBF88'0008) {
            skip = i - 1;
            break;
        }
    CHECK(skip < words.size());
    if (skip < words.size()) words[skip] = 0xBF80'0000;
    CHECK(refusal(words, false) == "a per-lane descriptor written where some lanes may keep the old one");
}

TEST(waterfall_a_buffer_descriptor_joined_per_lane_is_traced_on_the_paths_of_lanes_that_load) {
    const Resources r = waterfall(kWaterfallJoinedBufferWave32, false);
    const Descriptor* buffer = first_lane_phi(r);
    CHECK(buffer != nullptr);
    if (buffer == nullptr) return;
    const Source& outer = buffer->source;
    CHECK(outer.block == block_at(kWaterfallJoinedBufferWave32, 0x78));
    CHECK(outer.options.size() == 2);
    if (outer.options.size() != 2) return;
    CHECK(outer.options[0].has_value());
    CHECK(outer.options[1].has_value());
    if (!outer.options[0] || !outer.options[1]) return;
    const Source& flow = *outer.options[0];
    const Source& nested = *outer.options[1];
    CHECK(flow.kind == Source::Kind::LanePhi);
    CHECK(nested.kind == Source::Kind::LanePhi);
    CHECK(flow.options.size() == 2);
    if (flow.options.size() != 2) return;
    CHECK(!flow.options[0].has_value());
    CHECK(flow.options[1].has_value());
    if (!flow.options[1]) return;
    CHECK(table_offset(r, *flow.options[1]) == 0);
    CHECK(nested.block == block_at(kWaterfallJoinedBufferWave32, 0x74));
    CHECK(nested.options.size() == 2);
    if (nested.options.size() != 2) return;
    CHECK(!nested.options[0].has_value());
    CHECK(nested.options[1].has_value());
    if (!nested.options[1]) return;
    CHECK(table_offset(r, *nested.options[1]) == 0x20);
    CHECK(r.waterfall.compares.size() == 2);
    CHECK(r.waterfall.reads.size() == 4);
}

RootParam root(RootType::Kind kind, std::uint8_t user_data, std::uint8_t slot) {
    RootParam param_value;
    param_value.root = RootType{kind, 0};
    param_value.table_offset_dwords = 0;
    param_value.sgprs = kind == RootType::Kind::Descriptor ? 2 : 1;
    param_value.base_register = 0;
    param_value.user_data = user_data;
    param_value.slot = slot;
    return param_value;
}

UsedResource range(ResourceType::Kind kind, std::uint32_t table_offset_dwords, std::uint8_t dwords,
                   std::uint32_t stride, std::uint8_t user_data) {
    UsedResource used;
    used.kind = ResourceType{kind, 0};
    used.root = RootType{RootType::Kind::Table, 0};
    used.table_offset_dwords = table_offset_dwords;
    used.lower_bound = 0;
    used.stride = stride;
    used.dwords = dwords;
    used.user_data = user_data;
    used.slot = user_data;
    return used;
}

std::pair<std::vector<RootParam>, std::vector<UsedResource>> texture_tables() {
    return {{root(RootType::Kind::Table, 0, 0), root(RootType::Kind::Table, 1, 1)},
            {range(ResourceType::Kind::Srv, 0, 8, 0, 0), range(ResourceType::Kind::Sampler, 0, 4, 0, 1)}};
}

std::pair<std::vector<RootParam>, std::vector<UsedResource>> texture_and_uav_tables() {
    auto [layout, used] = texture_tables();
    used.push_back(range(ResourceType::Kind::Uav, 16, 4, 0, 0));
    return {layout, used};
}

Lowered lower_wave(const std::vector<std::uint32_t>& words, const std::vector<RootParam>& layout,
                   const std::vector<UsedResource>& used, bool wave64) {
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.wave64 = wave64;
    native.vgprs = 256;
    native.layout = layout;
    native.used = used;
    std::uint32_t sgprs = 0;
    for (const RootParam& entry : layout) sgprs += entry.sgprs;
    ComputeIo compute;
    compute.rsrc2 = 1u << 15 | std::max(sgprs, 2u) << 1;
    compute.threads = {1, 1, 1};
    native.io = compute;
    native.code = bytes_of(words);
    const Program p = build(native.code, wave64, 256);
    const Structured st = structure(p);
    const Resources res = track(p, native.layout, native.stage, &st);
    const Io raised = raise(p, native);
    Parts parts;
    parts.native = &native;
    parts.p = &p;
    parts.st = &st;
    parts.io = &raised;
    parts.res = &res;
    return lower(parts);
}

TEST(waterfall_loops_over_per_lane_descriptors_sample_with_each_lanes_own) {
    const std::array<std::pair<const std::vector<std::uint32_t>*, bool>, 3> cases = {
        std::pair<const std::vector<std::uint32_t>*, bool>{&kWaterfallFetchedWave32, false},
        std::pair<const std::vector<std::uint32_t>*, bool>{&kWaterfallFetchedWave64, true},
        std::pair<const std::vector<std::uint32_t>*, bool>{&kWaterfallPickedWave32, false}};
    for (const auto& [words, wave64] : cases) {
        const auto [layout, used] = texture_tables();
        const Lowered lowered = lower_wave(*words, layout, used, wave64);
        CHECK(lowered.nonuniform_handles == 1);
        CHECK(!lowered.wave_ops);
    }
}

TEST(waterfall_loops_over_descriptors_joined_per_lane_sample_with_each_lanes_own) {
    for (const std::vector<std::uint32_t>* words :
         {&kWaterfallJoinedWave32, &kWaterfallJoinedMixedWave32}) {
        const auto [layout, used] = texture_and_uav_tables();
        const Lowered lowered = lower_wave(*words, layout, used, false);
        CHECK(lowered.nonuniform_handles == 1);
        CHECK(!lowered.wave_ops);
    }
}

TEST(waterfall_gpu_dispatch_is_skipped_without_a_gpu_harness) {
    const auto [layout, used] = [] {
        std::vector<UsedResource> ranges = {range(ResourceType::Kind::Srv, 0, 8, 0, 0),
                                            range(ResourceType::Kind::Uav, 16, 4, 0, 0),
                                            range(ResourceType::Kind::Sampler, 0, 4, 0, 1)};
        return std::pair<std::vector<RootParam>, std::vector<UsedResource>>{texture_tables().first, ranges};
    }();
    std::uint32_t groups = 0;
    for (const std::vector<std::uint32_t>* words : {&kWaterfallJoinedWave32, &kWaterfallJoinedMixedWave32}) {
        const Lowered lowered = lower_wave(*words, layout, used, false);
        CHECK(lowered.nonuniform_handles == 1);
        CHECK(!lowered.wave_ops);
        ++groups;
    }
    const Lowered picked = lower_wave(kWaterfallPickedWave32, layout, used, false);
    CHECK(picked.nonuniform_handles == 1);
    ++groups;
    std::printf("  GPU dispatch skipped for %u waterfall programs: lanes are compared on a device, not here\n",
                groups);
}

}  // namespace
