// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "g6shim/shader/dxbc.hpp"
#include "g6shim/shader/dxil.hpp"
#include "g6shim/shader/error.hpp"
#include "g6shim/shader/inflate.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/scan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace g6shim::shader;

constexpr std::uint32_t kMarker = 0x000D'DCE7;
constexpr std::size_t kMarkerInRecord = 11;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void set_u32(std::vector<std::uint8_t>& data, std::size_t at, std::uint32_t value) {
    data[at] = static_cast<std::uint8_t>(value);
    data[at + 1] = static_cast<std::uint8_t>(value >> 8);
    data[at + 2] = static_cast<std::uint8_t>(value >> 16);
    data[at + 3] = static_cast<std::uint8_t>(value >> 24);
}

void set_u64(std::vector<std::uint8_t>& data, std::size_t at, std::uint64_t value) {
    set_u32(data, at, static_cast<std::uint32_t>(value));
    set_u32(data, at + 4, static_cast<std::uint32_t>(value >> 32));
}

std::vector<std::uint8_t> raw_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - at);
        const bool final = at + length == data.size();
        out.push_back(final ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
        out.push_back(static_cast<std::uint8_t>(inverse));
        out.push_back(static_cast<std::uint8_t>(inverse >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < data.size());
    return out;
}

class BitWriter {
public:
    void bit(unsigned value) {
        current_ = static_cast<std::uint8_t>(current_ | ((value & 1) << count_));
        if (++count_ == 8) {
            bytes_.push_back(current_);
            current_ = 0;
            count_ = 0;
        }
    }

    void code(std::uint32_t value, unsigned bits) {
        for (unsigned index = bits; index != 0; --index) bit((value >> (index - 1)) & 1);
    }

    std::vector<std::uint8_t> done() {
        if (count_ != 0) bytes_.push_back(current_);
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint8_t current_ = 0;
    unsigned count_ = 0;
};

std::vector<std::uint8_t> raw_fixed_literal(std::uint8_t literal) {
    BitWriter writer;
    writer.bit(1);
    writer.bit(1);
    writer.bit(0);
    writer.code(0x30u + literal, 8);
    writer.code(0, 7);
    return writer.done();
}

std::vector<std::uint8_t> raw_fixed_match() {
    BitWriter writer;
    writer.bit(1);
    writer.bit(1);
    writer.bit(0);
    writer.code(0x30u + static_cast<std::uint32_t>('A'), 8);
    writer.code(257 - 256, 7);
    writer.code(0, 5);
    writer.code(0, 7);
    return writer.done();
}

std::vector<std::uint8_t> raw_fixed_literals(const std::vector<std::uint8_t>& data) {
    BitWriter writer;
    writer.bit(1);
    writer.bit(1);
    writer.bit(0);
    for (const std::uint8_t byte : data) {
        if (byte <= 143) writer.code(0x30u + byte, 8);
        else writer.code(0x190u + (byte - 144), 9);
    }
    writer.code(0, 7);
    return writer.done();
}

std::vector<std::uint8_t> packed_record(const std::string& name, std::uint8_t kind,
                                        const std::vector<std::uint8_t>& payload) {
    const std::vector<std::uint8_t> packed = raw_fixed_literals(payload);
    const std::uint32_t n = static_cast<std::uint32_t>(packed.size() + 12);
    std::vector<std::uint8_t> out{0xF4, 0x01, 0x00, 0x00, static_cast<std::uint8_t>(name.size() + 1)};
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    out.push_back(kind);
    append_u32(out, n);
    append_u32(out, kMarker);
    append_u32(out, n);
    append_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

std::vector<std::uint8_t> dxbc(const std::vector<std::pair<FourCC, std::vector<std::uint8_t>>>& parts) {
    const std::size_t table = 0x20 + 4 * parts.size();
    std::size_t total = table;
    for (const auto& part : parts) total += 8 + part.second.size();
    std::vector<std::uint8_t> out{'D', 'X', 'B', 'C'};
    out.resize(20, 0);
    append_u32(out, 1);
    append_u32(out, static_cast<std::uint32_t>(total));
    append_u32(out, static_cast<std::uint32_t>(parts.size()));
    std::size_t offset = table;
    for (const auto& part : parts) {
        append_u32(out, static_cast<std::uint32_t>(offset));
        offset += 8 + part.second.size();
    }
    for (const auto& part : parts) {
        out.insert(out.end(), part.first.bytes.begin(), part.first.bytes.end());
        append_u32(out, static_cast<std::uint32_t>(part.second.size()));
        out.insert(out.end(), part.second.begin(), part.second.end());
    }
    return out;
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_u32(out, static_cast<std::uint32_t>(value));
    append_u32(out, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t kModulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kModulus;
        b = (b + a) % kModulus;
    }
    return b << 16 | a;
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - at);
        const bool final = at + length == data.size();
        out.push_back(final ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
        out.push_back(static_cast<std::uint8_t>(inverse));
        out.push_back(static_cast<std::uint8_t>(inverse >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < data.size());
    const std::uint32_t check = adler32(data);
    out.push_back(static_cast<std::uint8_t>(check >> 24));
    out.push_back(static_cast<std::uint8_t>(check >> 16));
    out.push_back(static_cast<std::uint8_t>(check >> 8));
    out.push_back(static_cast<std::uint8_t>(check));
    return out;
}

std::vector<std::uint8_t> xbch(const std::vector<std::uint8_t>& unpacked) {
    const std::vector<std::uint8_t> stream = zlib_stored(unpacked);
    std::vector<std::uint8_t> out{'X', 'B', 'C', 'H'};
    append_u32(out, 3);
    append_u64(out, stream.size());
    append_u64(out, unpacked.size());
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

std::size_t slot(HwStage stage) {
    constexpr std::array<std::pair<HwStage, std::size_t>, 8> kSlots{{
        {HwStage::Ls, 0x08}, {HwStage::Hs, 0x10}, {HwStage::Es, 0x18}, {HwStage::Gs, 0x20},
        {HwStage::Vs, 0x28}, {HwStage::Ps, 0x30}, {HwStage::Ms, 0x48}, {HwStage::Cs, 0x60},
    }};
    for (const auto& entry : kSlots)
        if (entry.first == stage) return entry.second;
    return 0;
}

constexpr std::size_t kStageDataSize = 0x14C + 8 * (4 + 10 * 10 * 4);
constexpr std::size_t kRecordSize = 0x54;
constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFULL;

std::vector<std::uint8_t> fssm(HwStage stage, const std::vector<std::uint8_t>& code) {
    std::vector<std::uint8_t> payload;
    append_u64(payload, 6);
    payload.resize(0x60 + 8, 0);
    constexpr std::array<std::size_t, 8> kSlotOffsets{0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60};
    for (const std::size_t at : kSlotOffsets) set_u64(payload, at, kAbsent);
    const std::uint64_t record = payload.size();
    set_u64(payload, slot(stage), record);
    const std::uint64_t code_at = record + kRecordSize + kStageDataSize;
    append_u64(payload, code_at);
    append_u64(payload, code.size());
    payload.resize(static_cast<std::size_t>(record) + 0x14, 0);
    append_u32(payload, stage == HwStage::Ps ? 1u << 5 : 0);
    payload.resize(static_cast<std::size_t>(record) + 0x50, 0);
    append_u32(payload, 28);
    payload.resize(static_cast<std::size_t>(record) + kRecordSize + kStageDataSize, 0);
    payload.insert(payload.end(), code.begin(), code.end());
    std::vector<std::uint8_t> out{'F', 'S', 'S', 'M'};
    append_u32(out, 0x101);
    append_u64(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> native_part(std::uint16_t kind, HwStage stage, const std::vector<std::uint8_t>& code) {
    const std::vector<std::uint8_t> blob = xbch(fssm(stage, code));
    std::vector<std::uint8_t> out(0x18, 0);
    out.insert(out.end(), {'X', 'D', 'A', 'T'});
    append_u32(out, 1);
    append_u32(out, 0x24);
    const std::size_t blob_at = out.size() + 12;
    append_u32(out, 0x0D);
    append_u32(out, static_cast<std::uint32_t>(blob_at));
    append_u32(out, static_cast<std::uint32_t>(blob.size()));
    out.insert(out.end(), blob.begin(), blob.end());
    const std::size_t bitcode_at = out.size();
    out.resize(bitcode_at + 16, 0);
    while (out.size() % 4 != 0) out.push_back(0);
    set_u32(out, 0, static_cast<std::uint32_t>(kind) << 16 | 0x66);
    set_u32(out, 4, static_cast<std::uint32_t>(out.size() / 4));
    out[8] = 'D';
    out[9] = 'X';
    out[10] = 'I';
    out[11] = 'L';
    set_u32(out, 12, 0x106);
    set_u32(out, 16, static_cast<std::uint32_t>(bitcode_at - 8));
    set_u32(out, 20, 16);
    return out;
}

std::vector<std::uint8_t> header_only_part(std::uint16_t kind) {
    std::vector<std::uint8_t> out(24, 0);
    set_u32(out, 0, static_cast<std::uint32_t>(kind) << 16 | 0x66);
    set_u32(out, 4, 6);
    out[8] = 'D';
    out[9] = 'X';
    out[10] = 'I';
    out[11] = 'L';
    set_u32(out, 12, 0x106);
    set_u32(out, 16, 16);
    set_u32(out, 20, 0);
    return out;
}

bool throws_parse(const std::function<void()>& action) {
    try {
        action();
    } catch (const ParseError&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(shader_tool_finds_stored_containers) {
    const std::vector<std::uint8_t> container = dxbc({{kSfi0, {1, 2, 3, 4}}});
    std::vector<std::uint8_t> file(5, 0xAA);
    file.insert(file.end(), container.begin(), container.end());
    file.insert(file.end(), {'D', 'X', 'B', 'C', 'x'});
    const std::vector<std::uint8_t> extended = file;

    const std::vector<StoredShaderContainer> found = stored_shader_containers(file);
    CHECK(found.size() == 1);
    if (found.size() == 1) {
        CHECK(found[0].offset == 5);
        CHECK(found[0].bytes.size() == container.size());
        CHECK(std::equal(found[0].bytes.begin(), found[0].bytes.end(), container.begin(), container.end()));
    }

    const std::vector<std::uint8_t> truncated(container.begin(), container.end() - 1);
    CHECK(stored_shader_containers(truncated).empty());

    std::vector<std::uint8_t> trailing = extended;
    trailing.push_back(0xEE);
    const std::vector<StoredShaderContainer> sized = stored_shader_containers(trailing);
    CHECK(sized.size() == 1);
    if (sized.size() == 1) CHECK(sized[0].bytes.size() == container.size());
}

TEST(shader_tool_reads_packed_shader_records) {
    const std::vector<std::uint8_t> container = dxbc({{kSfi0, {9, 8, 7, 6}}});
    const std::string name = "PS_Blit_0011AABB_Wrapped";
    const std::vector<std::uint8_t> record = packed_record(name, 0x32, container);
    const std::size_t marker = kMarkerInRecord + name.size();
    CHECK(marker < record.size() && record[marker] == 0xE7);

    std::vector<std::uint8_t> file = {0x5A, 0x5A, 0x5A, 0x5A};
    file.insert(file.end(), record.begin(), record.end());

    const std::vector<PackedShaderRecord> found = packed_shader_records(file);
    CHECK(found.size() == 1);
    if (found.size() == 1) {
        CHECK(found[0].offset == 4 + marker);
        CHECK(found[0].has_name);
        CHECK(found[0].name == name);
        CHECK(found[0].kind == 0x32);
        CHECK(found[0].unpacked_size == container.size());
        CHECK(found[0].packed.size() == record.size() - marker - 12);
        CHECK(found[0].unpack() == container);
    }

    const std::string long_name = "PS_" + std::string(40, 'x') + "_Wrapped";
    const std::vector<std::uint8_t> long_record = packed_record(long_name, 0x41, std::vector<std::uint8_t>(300, 7));
    std::vector<std::uint8_t> archive(0x100, 0x5A);
    archive.insert(archive.end(), long_record.begin(), long_record.end());
    const std::vector<PackedShaderRecord> long_found = packed_shader_records(archive);
    CHECK(long_found.size() == 1);
    if (long_found.size() == 1) {
        CHECK(long_found[0].has_name);
        CHECK(long_found[0].name == long_name);
        CHECK(long_found[0].unpack() == std::vector<std::uint8_t>(300, 7));
    }
}

TEST(shader_tool_ignores_bad_packed_records) {
    const std::string name = "VS_Blit_0011AABB_Wrapped";
    const std::vector<std::uint8_t> record =
        packed_record(name, 0x32, dxbc({{kSfi0, {1, 2, 3, 4}}}));
    const std::size_t marker = kMarkerInRecord + name.size();

    std::vector<std::uint8_t> disagreeing = record;
    set_u32(disagreeing, marker - 4, 9);
    set_u32(disagreeing, marker + 4, 8);
    CHECK(packed_shader_records(disagreeing).empty());

    std::vector<std::uint8_t> too_large = record;
    set_u32(too_large, marker + 8, static_cast<std::uint32_t>(kMaxPackedUnpacked + 1));
    CHECK(packed_shader_records(too_large).empty());

    std::vector<std::uint8_t> too_short = record;
    set_u32(too_short, marker - 4, 8);
    set_u32(too_short, marker + 4, 8);
    CHECK(packed_shader_records(too_short).empty());

    std::vector<std::uint8_t> truncated = record;
    truncated.resize(marker + 12);
    CHECK(packed_shader_records(truncated).empty());

    const std::vector<std::uint8_t> bare = {0xE7, 0xDC, 0x0D, 0x00};
    CHECK(packed_shader_records(bare).empty());
    CHECK(packed_shader_records({}).empty());
}

TEST(shader_tool_rejects_corrupt_packed_stream) {
    const std::string name = "CS_x";
    std::vector<std::uint8_t> record = packed_record(name, 0x3C, std::vector<std::uint8_t>(64, 1));
    record[kMarkerInRecord + name.size() + 12] = 0xFF;
    const std::vector<PackedShaderRecord> found = packed_shader_records(record);
    CHECK(found.size() == 1);
    if (found.size() == 1) CHECK(throws_parse([&] { found[0].unpack(); }));
}

TEST(shader_tool_inflates_raw_deflate) {
    std::vector<std::uint8_t> payload(300);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 7);
    const std::vector<std::uint8_t> stream = raw_stored(payload);
    CHECK(inflate_raw(stream, payload.size()) == payload);
    CHECK(inflate_raw(raw_stored({}), 0).empty());

    CHECK(throws_parse([&] { inflate_raw(stream, payload.size() - 1); }));
    std::vector<std::uint8_t> truncated = stream;
    truncated.pop_back();
    CHECK(throws_parse([&] { inflate_raw(truncated, payload.size()); }));
    CHECK(throws_parse([&] { inflate_raw(std::vector<std::uint8_t>{0x07}, 16); }));
}

TEST(shader_tool_inflates_fixed_deflate) {
    const std::vector<std::uint8_t> a = {'A'};
    const std::vector<std::uint8_t> zero = {0};
    const std::vector<std::uint8_t> matched = {'A', 'A', 'A', 'A'};
    CHECK(inflate_raw(raw_fixed_literal(static_cast<std::uint8_t>('A')), 1) == a);
    CHECK(inflate_raw(raw_fixed_literal(0), 1) == zero);
    CHECK(inflate_raw(raw_fixed_match(), 4) == matched);
    CHECK(throws_parse([] { inflate_raw(raw_fixed_match(), 3); }));
}

TEST(shader_tool_names_stages) {
    CHECK(shader_stage(shader_kind_from_raw(0)) == "ps");
    CHECK(shader_stage(shader_kind_from_raw(1)) == "vs");
    CHECK(shader_stage(shader_kind_from_raw(2)) == "gs");
    CHECK(shader_stage(shader_kind_from_raw(3)) == "hs");
    CHECK(shader_stage(shader_kind_from_raw(4)) == "ds");
    CHECK(shader_stage(shader_kind_from_raw(5)) == "cs");
    CHECK(shader_stage(shader_kind_from_raw(13)) == "ms");
    CHECK(shader_stage(shader_kind_from_raw(14)) == "as");
    CHECK(shader_stage(shader_kind_from_raw(6)) == "unknown");
    CHECK(shader_stage(shader_kind_from_raw(15)) == "unknown");
}

TEST(shader_tool_scans_and_deduplicates) {
    const std::vector<std::uint8_t> container = dxbc({{kSfi0, {1, 2, 3, 4}}});
    const std::vector<std::uint8_t> record = packed_record("VS_x", 0x32, container);
    std::vector<std::uint8_t> file = container;
    file.insert(file.end(), record.begin(), record.end());
    file.insert(file.end(), container.begin(), container.end());

    ShaderScanner scanner;
    const FileCounts counts = scanner.add_file(file);
    CHECK(counts.containers == 3);
    CHECK(counts.unique == 1);
    const ShaderTotals& totals = scanner.totals();
    CHECK(totals.containers == 3);
    CHECK(totals.unique == 1);
    CHECK(totals.files == 1);
    const auto stage = totals.stages.find("unknown");
    CHECK(stage != totals.stages.end());
    if (stage != totals.stages.end()) {
        CHECK(stage->second.total == 1);
        CHECK(stage->second.decoded == 0);
        CHECK(stage->second.translated == 0);
        CHECK(stage->second.failed == 1);
    }
    const auto failure = totals.failures.find("DXIL");
    CHECK(failure != totals.failures.end());
    if (failure != totals.failures.end()) CHECK(failure->second.count == 1);
}

TEST(shader_tool_reads_the_stage_with_placeholder_bitcode) {
    const std::vector<std::uint8_t> code{0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x81, 0xBF};
    const std::vector<std::uint8_t> part = native_part(1, HwStage::Vs, code);
    CHECK(throws_parse([&] { DxilProgram::parse(part); }));
    const std::vector<std::uint8_t> container = dxbc({{kDxil, part}});
    ShaderScanner scanner;
    const FileCounts counts = scanner.add_file(container);
    CHECK(counts.containers == 1);
    CHECK(counts.unique == 1);
    const ShaderTotals& totals = scanner.totals();
    const auto stage = totals.stages.find("vs");
    CHECK(stage != totals.stages.end());
    if (stage == totals.stages.end()) return;
    CHECK(stage->second.total == 1);
    CHECK(stage->second.failed == 0);
    CHECK(stage->second.decoded + stage->second.translated == 1);
    CHECK(totals.failures.find("DXIL") == totals.failures.end());
    CHECK(totals.failures.find("native") == totals.failures.end());
    CHECK(totals.failures.find("instruction") == totals.failures.end());
}

TEST(shader_tool_reports_each_shader_that_does_not_translate) {
    const std::vector<std::uint8_t> refused = dxbc({{kDxil, header_only_part(0)}});
    const std::vector<std::uint8_t> code{0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x81, 0xBF};
    const std::vector<std::uint8_t> translated = dxbc({{kDxil, native_part(1, HwStage::Vs, code)}});
    std::vector<std::uint8_t> file = refused;
    file.insert(file.end(), translated.begin(), translated.end());
    ShaderScanner scanner;
    std::vector<std::pair<std::string, std::string>> reported;
    std::size_t reported_bytes = 0;
    scanner.set_failure_reporter([&](std::span<const std::uint8_t> container, std::string_view stage,
                                     std::string_view why) {
        reported.emplace_back(std::string(stage), std::string(why));
        reported_bytes = container.size();
    });
    scanner.add_file(file);
    CHECK(reported.size() == 1);
    if (reported.size() != 1) return;
    CHECK(reported[0].first == "ps");
    CHECK(reported[0].second == "no native program");
    CHECK(reported_bytes == refused.size());
}

TEST(shader_tool_requires_a_native_program) {
    const std::vector<std::uint8_t> container = dxbc({{kDxil, header_only_part(0)}});
    ShaderScanner scanner;
    scanner.add_file(container);
    const ShaderTotals& totals = scanner.totals();
    const auto stage = totals.stages.find("ps");
    CHECK(stage != totals.stages.end());
    if (stage == totals.stages.end()) return;
    CHECK(stage->second.total == 1);
    CHECK(stage->second.decoded == 0);
    CHECK(stage->second.failed == 1);
    const auto failure = totals.failures.find("native");
    CHECK(failure != totals.failures.end());
    if (failure != totals.failures.end()) CHECK(failure->second.first == "no native program");
}

TEST(shader_tool_requires_a_decodable_native_program) {
    const std::vector<std::uint8_t> code{0x00, 0x00, 0x80, 0xBF, 0x00};
    const std::vector<std::uint8_t> container = dxbc({{kDxil, native_part(5, HwStage::Cs, code)}});
    ShaderScanner scanner;
    scanner.add_file(container);
    const ShaderTotals& totals = scanner.totals();
    const auto stage = totals.stages.find("cs");
    CHECK(stage != totals.stages.end());
    if (stage == totals.stages.end()) return;
    CHECK(stage->second.failed == 1);
    CHECK(stage->second.decoded == 0);
    const auto failure = totals.failures.find("instruction");
    CHECK(failure != totals.failures.end());
}

TEST(shader_tool_counts_translation_failures_by_reason) {
    const std::vector<std::uint8_t> code{0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x81, 0xBF};
    const std::vector<std::uint8_t> container = dxbc({{kDxil, native_part(1, HwStage::Es, code)}});
    ShaderScanner scanner;
    scanner.add_file(container);
    const ShaderTotals& totals = scanner.totals();
    const auto stage = totals.stages.find("vs");
    CHECK(stage != totals.stages.end());
    if (stage == totals.stages.end()) return;
    CHECK(stage->second.decoded == 1);
    CHECK(stage->second.translated == 0);
    CHECK(stage->second.failed == 0);
    const auto failure = totals.failures.find("translation: I/O of this stage is not supported");
    CHECK(failure != totals.failures.end());
    if (failure != totals.failures.end()) CHECK(failure->second.count == 1);
}

TEST(shader_tool_hands_translated_containers_to_the_writer) {
    const std::vector<std::uint8_t> code{0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x81, 0xBF};
    const std::vector<std::uint8_t> container = dxbc({{kDxil, native_part(1, HwStage::Vs, code)}});
    ShaderScanner scanner;
    std::string hash;
    std::vector<std::uint8_t> dxil;
    scanner.set_translated_writer([&](std::string_view name, std::span<const std::uint8_t> bytes) {
        hash = std::string(name);
        dxil.assign(bytes.begin(), bytes.end());
    });
    scanner.add_file(container);
    const ShaderTotals& totals = scanner.totals();
    const auto stage = totals.stages.find("vs");
    CHECK(stage != totals.stages.end());
    if (stage == totals.stages.end()) return;
    CHECK(stage->second.translated == 1);
    CHECK(hash.size() == 16);
    CHECK(std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
    CHECK(dxil.size() > 4);
    CHECK(std::equal(dxil.begin(), dxil.begin() + 4, std::array<std::uint8_t, 4>{'D', 'X', 'B', 'C'}.begin()));
}

TEST(shader_tool_visits_each_unique_container_once) {
    const std::vector<std::uint8_t> first = dxbc({{kSfi0, {1, 2, 3, 4}}});
    const std::vector<std::uint8_t> second = dxbc({{kSfi0, {5, 6, 7, 8}}});
    std::vector<std::uint8_t> file = first;
    file.insert(file.end(), second.begin(), second.end());
    file.insert(file.end(), first.begin(), first.end());

    ShaderScanner scanner;
    std::mutex visited_mutex;
    std::vector<std::vector<std::uint8_t>> visited;
    scanner.set_container_visitor([&](std::span<const std::uint8_t> container) {
        const std::lock_guard<std::mutex> lock(visited_mutex);
        visited.emplace_back(container.begin(), container.end());
    });
    scanner.add_file(file);
    scanner.add_file(second);
    CHECK(visited.size() == 2);
    CHECK(std::count(visited.begin(), visited.end(), first) == 1);
    CHECK(std::count(visited.begin(), visited.end(), second) == 1);
}
