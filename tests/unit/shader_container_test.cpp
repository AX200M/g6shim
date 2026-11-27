// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "g6shim/shader/dxbc.hpp"
#include "g6shim/shader/dxil.hpp"
#include "g6shim/shader/error.hpp"
#include "g6shim/shader/native.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace g6shim::shader;

constexpr std::uint32_t kKindNative = 0x0D;
constexpr std::uint32_t kXbchVersion = 3;
constexpr std::size_t kXbchHeader = 0x18;
constexpr std::uint32_t kFssmVersion = 0x101;
constexpr std::size_t kFssmHeader = 0x10;
constexpr std::size_t kXdatOffset = 0x18;
constexpr std::uint64_t kGraphicsSlotCount = 6;
constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFULL;
constexpr std::size_t kRecordSize = 0x54;
constexpr std::size_t kRecordFlags = 0x14;
constexpr std::uint32_t kFlagWave32 = 1U << 5;
constexpr std::size_t kRecordRegs = 0x50;
constexpr std::size_t kRecordLayout = 0x28;
constexpr std::size_t kRecordUsed = 0x30;
constexpr std::size_t kRecordCounts = 0x38;
constexpr std::size_t kRecordLds = 0x4C;
constexpr std::size_t kPsInputs = 0x24;
constexpr std::size_t kPsInputCount = 0x124;
constexpr std::size_t kPsInputAddr = 0x134;
constexpr std::size_t kPsPatchSize = 0x148;
constexpr std::size_t kPsPatches = 0x14C;
constexpr std::size_t kPatchSlotDwords = 10;
constexpr std::size_t kPatchFull = 9;
constexpr std::size_t kPatchBytes = 4 + 10 * kPatchSlotDwords * 4;
constexpr std::size_t kMrts = 8;
constexpr std::size_t kVsOutputs = 0xC8;
constexpr std::size_t kVsOutputCount = 0x188;
constexpr std::size_t kMsClipCntl = 0x194;
constexpr std::size_t kMsInfo = 0x448;
constexpr std::size_t kStageDataSize = kPsPatches + kMrts * kPatchBytes;
constexpr std::uint32_t kVgprs = 28;

constexpr std::array<std::pair<HwStage, std::size_t>, 8> kSlots = {{
    {HwStage::Ls, 0x08},
    {HwStage::Hs, 0x10},
    {HwStage::Es, 0x18},
    {HwStage::Gs, 0x20},
    {HwStage::Vs, 0x28},
    {HwStage::Ps, 0x30},
    {HwStage::Ms, 0x48},
    {HwStage::Cs, 0x60},
}};

const std::vector<std::uint8_t> kVsCode = {1, 2, 3, 4, 5, 6, 7, 8};
const std::vector<std::uint8_t> kPsCode = {9, 10, 11, 12};

std::size_t slot(HwStage stage) {
    for (const auto& entry : kSlots) {
        if (entry.first == stage) return entry.second;
    }
    return 0;
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_u32(out, static_cast<std::uint32_t>(value));
    append_u32(out, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& data, std::size_t at) {
    return read_u32(data, at) | static_cast<std::uint64_t>(read_u32(data, at + 4)) << 32;
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

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
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
    std::vector<std::uint8_t> out = bytes_of("XBCH");
    append_u32(out, kXbchVersion);
    append_u64(out, stream.size());
    append_u64(out, unpacked.size());
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

std::vector<std::uint8_t> empty_stage_data() {
    std::vector<std::uint8_t> data(kStageDataSize, 0);
    for (std::size_t mrt = 0; mrt < kMrts; ++mrt) set_u32(data, kPsPatches + mrt * kPatchBytes, UINT32_MAX);
    return data;
}

std::vector<std::uint8_t> fssm(const std::vector<std::pair<HwStage, std::vector<std::uint8_t>>>& programs) {
    std::vector<std::uint8_t> payload;
    append_u64(payload, kGraphicsSlotCount);
    payload.resize(slot(HwStage::Cs) + 8, 0);
    for (const auto& entry : kSlots) set_u64(payload, entry.second, kAbsent);
    for (const auto& [stage, code] : programs) {
        const std::size_t offset = slot(stage);
        const std::uint64_t record = payload.size();
        set_u64(payload, offset, record);
        const std::uint64_t code_at = record + kRecordSize + kStageDataSize;
        append_u64(payload, code_at);
        append_u64(payload, code.size());
        payload.resize(record + kRecordFlags, 0);
        append_u32(payload, stage == HwStage::Ps ? kFlagWave32 : 0);
        payload.resize(record + kRecordRegs, 0);
        append_u32(payload, kVgprs | 31U << 20);
        const std::vector<std::uint8_t> stage_data = empty_stage_data();
        payload.insert(payload.end(), stage_data.begin(), stage_data.end());
        payload.insert(payload.end(), code.begin(), code.end());
    }
    std::vector<std::uint8_t> out = bytes_of("FSSM");
    append_u32(out, kFssmVersion);
    append_u64(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::size_t stage_data_at(const std::vector<std::uint8_t>& blob, HwStage stage) {
    return kFssmHeader + static_cast<std::size_t>(read_u64(blob, kFssmHeader + slot(stage))) + kRecordSize;
}

std::vector<std::uint8_t> part(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& blobs) {
    std::vector<std::uint8_t> out(kXdatOffset, 0);
    out.insert(out.end(), {'X', 'D', 'A', 'T'});
    append_u32(out, static_cast<std::uint32_t>(blobs.size()));
    append_u32(out, 0x24);
    std::size_t offset = out.size() + 12 * blobs.size();
    for (const auto& [kind, blob] : blobs) {
        append_u32(out, kind);
        append_u32(out, static_cast<std::uint32_t>(offset));
        append_u32(out, static_cast<std::uint32_t>(blob.size()));
        offset += blob.size();
    }
    for (const auto& [kind, blob] : blobs) out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

std::vector<std::uint8_t> dxbc_build(const std::vector<std::pair<FourCC, std::vector<std::uint8_t>>>& parts) {
    const std::size_t table = 0x20 + 4 * parts.size();
    std::size_t total = table;
    for (const auto& part : parts) total += 8 + part.second.size();
    std::vector<std::uint8_t> out = bytes_of("DXBC");
    out.resize(20, 0);
    append_u32(out, 1);
    append_u32(out, static_cast<std::uint32_t>(total));
    append_u32(out, static_cast<std::uint32_t>(parts.size()));
    std::size_t offset = table;
    for (const auto& part : parts) {
        append_u32(out, static_cast<std::uint32_t>(offset));
        offset += 8 + part.second.size();
    }
    for (const auto& [fourcc, data] : parts) {
        out.insert(out.end(), fourcc.bytes.begin(), fourcc.bytes.end());
        append_u32(out, static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

std::vector<std::uint8_t> fake_bitcode() {
    std::vector<std::uint8_t> out = {0x42, 0x43, 0xC0, 0xDE};
    out.resize(16, 0xAA);
    return out;
}

std::vector<std::uint8_t> build_standard(std::uint16_t kind, std::pair<std::uint8_t, std::uint8_t> model,
                                         std::uint32_t dxil_version, const std::vector<std::uint8_t>& bitcode) {
    const std::uint32_t version = static_cast<std::uint32_t>(kind) << 16 | static_cast<std::uint32_t>(model.first) << 4 |
                                  model.second;
    const std::size_t padded = (bitcode.size() + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> out;
    append_u32(out, version);
    append_u32(out, static_cast<std::uint32_t>((24 + padded) / 4));
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    append_u32(out, dxil_version);
    append_u32(out, 16);
    append_u32(out, static_cast<std::uint32_t>(bitcode.size()));
    out.insert(out.end(), bitcode.begin(), bitcode.end());
    out.resize(24 + padded, 0);
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

NativeProgram program(HwStage stage, bool wave64, StageIo io, const std::vector<std::uint8_t>& code) {
    NativeProgram p;
    p.stage = stage;
    p.wave64 = wave64;
    p.vgprs = static_cast<std::uint16_t>(kVgprs);
    p.io = std::move(io);
    p.code = code;
    return p;
}

VsOutput output(std::uint8_t row, std::uint8_t param) {
    return VsOutput{usage_from_raw(6), row, param, false, false};
}

}  // namespace

TEST(shader_dxbc_round_trips_parts) {
    const std::vector<std::uint8_t> bytes = dxbc_build({{kSfi0, {1, 2, 3, 4}}, {kHash, std::vector<std::uint8_t>(20, 9)}});
    const Container container = Container::parse(bytes);
    CHECK(container.parts.size() == 2);
    CHECK(container.part(kSfi0) != nullptr);
    if (container.part(kSfi0) != nullptr) {
        const auto data = container.part(kSfi0)->data;
        CHECK(data.size() == 4);
        CHECK(data.size() == 4 && data[0] == 1 && data[1] == 2 && data[2] == 3 && data[3] == 4);
    }
    CHECK(container.part(kHash) != nullptr);
    CHECK(container.part(kHash)->data.size() == 20);
    CHECK(container.part(kDxil) == nullptr);
}

TEST(shader_dxbc_rejects_bad_input) {
    bool not_dxbc = false;
    try {
        Container::parse(bytes_of("nope"));
    } catch (const ParseError& error) {
        not_dxbc = error.kind() == ErrorKind::NotDxbc;
    }
    CHECK(not_dxbc);
    std::vector<std::uint8_t> bytes = dxbc_build({{kSfi0, std::vector<std::uint8_t>(8, 0)}});
    const std::size_t last = bytes.size() - 1;
    bytes[0x20] = 0xFF;
    CHECK(throws_parse([&] { Container::parse(bytes); }));
    bytes.resize(last);
    CHECK(throws_parse([&] { Container::parse(bytes); }));
}

TEST(shader_dxil_parses_standard_layout) {
    const std::vector<std::uint8_t> bytes =
        build_standard(0, {std::uint8_t{6}, std::uint8_t{0}}, 0x100, fake_bitcode());
    const DxilProgram program = DxilProgram::parse(bytes);
    CHECK(program.kind.id == ShaderKind::Id::Pixel);
    CHECK(program.shader_model ==
          (std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{6}, std::uint8_t{0}}));
    CHECK(program.is_standard_layout());
    CHECK(program.xbox_extras() == XboxExtras{});
}

TEST(shader_dxil_detects_xbox_layout) {
    const std::vector<std::uint8_t> bitcode = fake_bitcode();
    const std::vector<std::uint8_t> xdat = bytes_of(std::string_view("XDAT\x01\x00\x00\x00", 8));
    const std::vector<std::uint8_t> trailer = bytes_of(std::string_view("xbop\x02\x00\x00\x00", 8));
    const std::size_t size = 24 + xdat.size() + bitcode.size() + trailer.size();
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, (5U << 16) | 0x65);
    append_u32(bytes, static_cast<std::uint32_t>(size / 4));
    bytes.insert(bytes.end(), {'D', 'X', 'I', 'L'});
    append_u32(bytes, 0x105);
    append_u32(bytes, static_cast<std::uint32_t>(16 + xdat.size()));
    append_u32(bytes, static_cast<std::uint32_t>(bitcode.size()));
    bytes.insert(bytes.end(), xdat.begin(), xdat.end());
    bytes.insert(bytes.end(), bitcode.begin(), bitcode.end());
    bytes.insert(bytes.end(), trailer.begin(), trailer.end());

    const DxilProgram program = DxilProgram::parse(bytes);
    CHECK(program.kind.id == ShaderKind::Id::Compute);
    CHECK(program.shader_model ==
          (std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{6}, std::uint8_t{5}}));
    CHECK(!program.is_standard_layout());
    CHECK(program.xbox_extras() == (XboxExtras{true, true}));
    CHECK(std::equal(program.bitcode.begin(), program.bitcode.end(), bitcode.begin(), bitcode.end()));
}

TEST(shader_dxil_rejects_missing_bitcode_magic) {
    const std::vector<std::uint8_t> bytes =
        build_standard(0, {std::uint8_t{6}, std::uint8_t{0}}, 0x100, std::vector<std::uint8_t>(16, 0));
    CHECK(throws_parse([&] { DxilProgram::parse(bytes); }));
}

TEST(shader_native_finds_graphics_programs) {
    const std::vector<std::uint8_t> native = xbch(fssm({{HwStage::Vs, kVsCode}, {HwStage::Ps, kPsCode}}));
    const std::vector<std::uint8_t> data = part({{0x0A, bytes_of("xbop")}, {kKindNative, native}});
    const std::vector<NativeProgram> found = native_programs(data);
    const std::vector<NativeProgram> expected = {
        program(HwStage::Vs, true, StageIo{VsIo{}}, kVsCode),
        program(HwStage::Ps, false, StageIo{PsIo{}}, kPsCode),
    };
    CHECK(found == expected);
}

TEST(shader_native_reads_root_tables) {
    std::vector<std::uint8_t> blob = fssm({{HwStage::Ps, kPsCode}});
    const std::size_t record =
        kFssmHeader + static_cast<std::size_t>(read_u64(blob, kFssmHeader + slot(HwStage::Ps)));
    const std::uint64_t tables = blob.size() - kFssmHeader;
    for (std::uint32_t value : {3U, 2U, 1U, 5U}) append_u32(blob, value);
    for (std::uint8_t value : std::array<std::uint8_t, 8>{9, 1, 0, 0, 0, 3, 0, 0}) blob.push_back(value);
    for (std::uint32_t value : {2U, 5U, 4U}) append_u32(blob, value);
    for (std::uint8_t value : std::array<std::uint8_t, 4>{8, 9, 1, 0}) blob.push_back(value);
    const std::uint64_t payload = blob.size() - kFssmHeader;
    set_u64(blob, 8, payload);
    set_u64(blob, record + kRecordLayout, tables);
    set_u64(blob, record + kRecordUsed, tables + 20);
    set_u32(blob, record + kRecordCounts, 1);
    set_u32(blob, record + kRecordCounts + 4, 1);

    const std::vector<NativeProgram> found = native_programs(part({{kKindNative, xbch(blob)}}));
    CHECK(found.size() == 1);
    if (found.size() != 1) return;
    const std::vector<RootParam> layout = {
        RootParam{root_type_from_raw(3), 2, 1, 5, 9, 1},
    };
    const std::vector<UsedResource> used = {
        UsedResource{ResourceType{ResourceType::Kind::Srv, 0}, root_type_from_raw(3), 2, 5, 4, 8, 9, 1},
    };
    CHECK(found[0].layout == layout);
    CHECK(found[0].used == used);

    blob[record + kRecordCounts] = 9;
    CHECK(throws_parse([&] { native_programs(part({{kKindNative, xbch(blob)}})); }));
}

TEST(shader_native_reads_pixel_io_and_applies_the_full_export_patch) {
    std::vector<std::uint8_t> code;
    for (std::uint32_t word : {0xBF80'0000U, 0xBF80'0000U, 0xBF80'0000U, 0xBF80'0000U}) append_u32(code, word);
    std::vector<std::uint8_t> blob = fssm({{HwStage::Ps, code}});
    const std::size_t e = stage_data_at(blob, HwStage::Ps);
    for (std::size_t i = 0; i < 8; ++i) blob[e + kPsInputs + i] = std::array<std::uint8_t, 8>{6, 1, 3, 0, 0, 0, 1, 1}[i];
    set_u32(blob, e + kPsInputCount, 1);
    set_u32(blob, e + kPsInputAddr, 2);
    set_u32(blob, e + kPsPatchSize, 2);
    const std::size_t patch = e + kPsPatches + kPatchBytes;
    set_u32(blob, patch, 1);
    const std::size_t full = patch + 4 + kPatchFull * kPatchSlotDwords * 4;
    for (std::size_t i = 0; i < 8; ++i)
        blob[full + i] = std::array<std::uint8_t, 8>{0x1F, 0x18, 0, 0xF8, 0, 1, 2, 3}[i];

    const std::vector<NativeProgram> found = native_programs(part({{kKindNative, xbch(blob)}}));
    CHECK(found.size() == 1);
    if (found.size() != 1) return;
    const auto* ps = std::get_if<PsIo>(&found[0].io);
    CHECK(ps != nullptr);
    if (ps == nullptr) return;
    const PsInput input = PsInput{usage_from_raw(6), 1, 3, 0, true, false, false, 0};
    CHECK(ps->inputs == std::vector<PsInput>{input});
    CHECK(ps->input_addr == 2);
    CHECK(ps->patches.size() == 1);
    if (ps->patches.size() == 1) {
        CHECK(ps->patches[0].mrt == 1);
        CHECK(ps->patches[0].offset == 1);
    }
    const std::vector<std::uint8_t> patched = found[0].patched_code();
    CHECK(std::equal(patched.begin(), patched.begin() + 4, found[0].code.begin()));
    CHECK(std::equal(patched.begin() + 4, patched.begin() + 12, std::array<std::uint8_t, 8>{0x1F, 0x18, 0, 0xF8, 0, 1, 2, 3}.begin()));
    CHECK(std::equal(patched.begin() + 12, patched.end(), found[0].code.begin() + 12));

    set_u32(blob, patch, 3);
    CHECK(throws_parse([&] { native_programs(part({{kKindNative, xbch(blob)}})); }));
}

TEST(shader_native_finds_mesh_and_compute_programs) {
    for (HwStage stage : {HwStage::Ms, HwStage::Cs}) {
        const std::vector<NativeProgram> found =
            native_programs(part({{kKindNative, xbch(fssm({{stage, kVsCode}}))}}));
        CHECK(found.size() == 1);
        if (found.size() == 1) CHECK(found[0].stage == stage);
    }
}

TEST(shader_native_reads_mesh_outputs_and_limits) {
    std::vector<std::uint8_t> blob = fssm({{HwStage::Ms, kVsCode}});
    const std::size_t e = stage_data_at(blob, HwStage::Ms);
    for (std::size_t i = 0; i < 12; ++i)
        blob[e + kVsOutputs + i] =
            std::array<std::uint8_t, 12>{6, 1, 0, 0, 0, 0, 6, 0, 0x82, 0, 0, 0}[i];
    set_u32(blob, e + kVsOutputCount, 2);
    set_u32(blob, e + kMsClipCntl, 0x0040'000F);
    for (std::size_t i = 0; i < 8; ++i)
        blob[e + kMsInfo + i] = std::array<std::uint8_t, 8>{0x80, 0, 0x80, 0, 0x40, 0, 13, 2}[i];
    const std::size_t record = e - kRecordSize;
    set_u32(blob, record + kRecordLds, 16);

    const std::vector<NativeProgram> found = native_programs(part({{kKindNative, xbch(blob)}}));
    CHECK(found.size() == 1);
    if (found.size() != 1) return;
    const MeshIo expected = MeshIo{{output(1, 0), output(0, 0x82)}, 0x0040'000F, 128, 128, 64, 2, 16};
    CHECK(found[0].io == StageIo{expected});
}

TEST(shader_native_compute_dispatch_settings) {
    const ComputeIo cs = ComputeIo{0x1098A, {16, 16, 1}};
    CHECK(cs.user_sgprs() == 5);
    CHECK(cs.system_sgprs() == (std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>>{
                                   {std::uint8_t{5}, std::uint8_t{6}, std::nullopt}, std::nullopt}));
    CHECK(cs.thread_id_vgprs() == 2);
    CHECK(cs.lds_bytes() == 1024);
    const ComputeIo cs2 = ComputeIo{0x138C, {8, 8, 1}};
    CHECK(cs2.system_sgprs() == (std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>>{
                                    {std::uint8_t{6}, std::uint8_t{7}, std::uint8_t{8}}, std::nullopt}));
    CHECK(cs2.thread_id_vgprs() == 3);
    CHECK(cs2.lds_bytes() == 0);
}

TEST(shader_native_initialized_sgprs_covers_user_system_and_scratch) {
    const ComputeIo cs = ComputeIo{0x1098A, {16, 16, 1}};
    CHECK(cs.initialized_sgprs() == 7);
    const ComputeIo all_three = ComputeIo{0x78C, {8, 8, 1}};
    CHECK(all_three.initialized_sgprs() == 10);
    const ComputeIo scratch = ComputeIo{0x78D, {8, 8, 1}};
    CHECK(scratch.initialized_sgprs() == 11);
}

TEST(shader_native_reads_every_native_blob) {
    const std::vector<std::uint8_t> es = xbch(fssm({{HwStage::Es, kVsCode}}));
    const std::vector<std::uint8_t> vs = xbch(fssm({{HwStage::Vs, kVsCode}}));
    const std::vector<std::uint8_t> data =
        part({{0x12, std::vector<std::uint8_t>(16, 0)}, {kKindNative, es}, {kKindNative, vs}});
    const std::vector<NativeProgram> found = native_programs(data);
    std::vector<HwStage> stages;
    for (const NativeProgram& program : found) stages.push_back(program.stage);
    CHECK(stages == (std::vector<HwStage>{HwStage::Es, HwStage::Vs}));
}

TEST(shader_native_pc_layout_has_none) {
    CHECK(native_programs(std::vector<std::uint8_t>(64, 0)).empty());
    CHECK(native_programs(part({{0x0A, bytes_of("xbop")}})).empty());
}

TEST(shader_native_rejects_corrupt_blobs) {
    const std::vector<std::uint8_t> good = xbch(fssm({{HwStage::Ls, kVsCode}}));
    const auto corrupt = [&](std::size_t at, std::uint8_t byte) {
        std::vector<std::uint8_t> blob = good;
        blob[at] = byte;
        return throws_parse([&] { native_programs(part({{kKindNative, blob}})); });
    };
    CHECK(corrupt(4, 9));
    CHECK(corrupt(16, 0xFF));
    CHECK(corrupt(kXbchHeader + 2, 0xFF));
    CHECK(throws_parse([&] {
        native_programs(part({{kKindNative, std::vector<std::uint8_t>(good.begin(), good.begin() + 20)}}));
    }));

    std::vector<std::uint8_t> bad_record = fssm({{HwStage::Ls, kVsCode}});
    const std::size_t at = kFssmHeader + slot(HwStage::Ls);
    set_u64(bad_record, at, UINT64_MAX);
    CHECK(throws_parse([&] { native_programs(part({{kKindNative, xbch(bad_record)}})); }));
}
