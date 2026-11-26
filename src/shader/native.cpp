// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/native.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/inflate.hpp"
#include "reader.hpp"

namespace g6shim::shader {

namespace {

constexpr std::size_t kXdatOffset = 0x18;
constexpr std::size_t kMaxXdatEntries = 64;
constexpr std::uint32_t kKindNative = 0x0D;
constexpr std::string_view kFspxMagic = "FSPX";
constexpr std::uint32_t kFspxVersion = 1;
constexpr std::size_t kFspxHeader = 0x10;
constexpr std::size_t kFspxEntry = 16;
constexpr std::uint32_t kFspxPacket = 0;
constexpr std::uint32_t kFspxCode = 6;
constexpr std::uint32_t kFspxMetadata = 7;
constexpr std::size_t kFspxCodeDwords = 8;
constexpr std::size_t kFspxRecords = 0x18;
constexpr std::size_t kFspxRecord = 32;
constexpr std::uint8_t kFspxResourceRecord = 0x41;
constexpr std::uint8_t kFspxViewRecord = 0x42;
constexpr std::uint8_t kFspxHashRecord = 0x43;
constexpr std::size_t kFspxResourceType = 4;
constexpr std::size_t kFspxRootType = 5;
constexpr std::size_t kFspxTableOffset = 8;
constexpr std::size_t kFspxRegister = 12;
constexpr std::size_t kFspxDescriptorDwords = 20;
constexpr std::size_t kFspxUserData = 21;
constexpr std::size_t kFspxRootSlot = 22;
constexpr std::size_t kPacketThreads = 0x0c;
constexpr std::size_t kPacketFlags = 0x12;
constexpr std::uint16_t kPacketWave32 = 1 << 4;
constexpr std::size_t kPacketRsrc1 = 0x14;
constexpr std::size_t kPacketRsrc2 = 0x18;
constexpr std::uint32_t kVgprGranuleMask = 0x3F;
constexpr std::uint32_t kXbchVersion = 3;
constexpr std::size_t kXbchHeader = 0x18;
constexpr std::uint32_t kFssmVersion = 0x101;
constexpr std::size_t kFssmHeader = 0x10;
constexpr std::uint64_t kGraphicsSlotCount = 6;
constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEF;
constexpr std::uint64_t kRecordSize = 0x54;
constexpr std::size_t kRecordFlags = 0x14;
constexpr std::uint32_t kFlagWave32 = 1u << 5;
constexpr std::size_t kRecordRegs = 0x50;
constexpr std::size_t kRecordLayout = 0x28;
constexpr std::size_t kRecordUsed = 0x30;
constexpr std::size_t kRecordCounts = 0x38;
constexpr std::size_t kEntrySize = 20;
constexpr std::uint32_t kMaxSemantics = 32;
constexpr std::size_t kPsInputs = 0x24;
constexpr std::size_t kPsInputCount = 0x124;
constexpr std::size_t kPsInputAddr = 0x134;
constexpr std::size_t kPsDbShaderControl = 0x138;
constexpr std::size_t kPsPatchSize = 0x148;
constexpr std::size_t kPsPatches = 0x14C;
constexpr std::size_t kPatchSlotDwords = 10;
constexpr std::size_t kPatchFull = 9;
constexpr std::size_t kPatchBytes = 4 + 10 * kPatchSlotDwords * 4;
constexpr std::size_t kMrts = 8;
constexpr std::size_t kVsInputs = 0x24;
constexpr std::size_t kVsInputCount = 0xC4;
constexpr std::size_t kVsOutputs = 0xC8;
constexpr std::size_t kVsOutputCount = 0x188;
constexpr std::size_t kCsRsrc2 = 0x24;
constexpr std::size_t kCsThreads = 0x28;
constexpr std::size_t kRecordLds = 0x4C;
constexpr std::size_t kMsClipCntl = 0x194;
constexpr std::size_t kMsInfo = 0x448;
constexpr std::uint64_t kMaxUnpacked = 16ull << 20;
constexpr std::uint32_t kMaxVgprs = 256;

struct Slot {
    HwStage stage;
    std::size_t at;
};

constexpr std::array<Slot, 8> kSlots{{
    {HwStage::Ls, 0x08},
    {HwStage::Hs, 0x10},
    {HwStage::Es, 0x18},
    {HwStage::Gs, 0x20},
    {HwStage::Vs, 0x28},
    {HwStage::Ps, 0x30},
    {HwStage::Ms, 0x48},
    {HwStage::Cs, 0x60},
}};

std::span<const std::uint8_t> unwrap(std::optional<std::span<const std::uint8_t>> range, const char* reason) {
    if (!range) detail::fail_malformed(reason);
    return *range;
}

std::span<const std::uint8_t> typed(std::span<const std::uint8_t> data, std::size_t at, std::size_t size, std::size_t count_at) {
    const std::uint32_t count = detail::read_u32(data, count_at);
    if (count > kMaxSemantics) detail::fail_malformed("implausible semantic count");
    const std::uint64_t len = static_cast<std::uint64_t>(size) * count;
    if (at > data.size() || len > data.size() - at) detail::fail_malformed("stage data out of bounds");
    return data.subspan(at, static_cast<std::size_t>(len));
}

bool starts_with(std::span<const std::uint8_t> data, std::string_view magic) {
    return data.size() >= magic.size() && std::memcmp(data.data(), magic.data(), magic.size()) == 0;
}

std::uint32_t entry_u32(std::span<const std::uint8_t> entry, std::size_t at) { return detail::read_u32(entry, at); }

std::vector<std::uint8_t> unpack(std::span<const std::uint8_t> blob) {
    if (!starts_with(blob, "XBCH")) detail::fail_malformed("native blob lacks XBCH magic");
    if (detail::read_u32(blob, 4) != kXbchVersion) detail::fail_malformed("unknown XBCH version");
    const std::uint64_t packed = detail::read_u64(blob, 8);
    const std::uint64_t unpacked = detail::read_u64(blob, 16);
    if (unpacked > kMaxUnpacked) detail::fail_malformed("implausible XBCH unpacked size");
    const auto stream = unwrap(detail::bytes(blob, kXbchHeader, packed), "XBCH stream out of bounds");
    std::vector<std::uint8_t> out;
    try {
        out = inflate_zlib(stream, static_cast<std::size_t>(unpacked));
    } catch (const ParseError&) {
        detail::fail_malformed("XBCH stream does not inflate to its stated size");
    }
    if (out.size() != unpacked) detail::fail_malformed("XBCH stream does not inflate to its stated size");
    return out;
}

VsIo read_vs_io(std::span<const std::uint8_t> data);

StageIo ps_io(std::span<const std::uint8_t> data, std::size_t code_len) {
    PsIo io;
    const auto input_bytes = typed(data, kPsInputs, 8, kPsInputCount);
    for (std::size_t i = 0; i < input_bytes.size(); i += 8) {
        const auto entry = input_bytes.subspan(i, 8);
        PsInput input;
        input.usage = usage_from_raw(entry[0]);
        input.row = entry[1];
        input.mask = static_cast<std::uint8_t>(entry[2] & 0xF);
        input.attr = entry[3];
        input.flat = (entry[7] & 1) != 0;
        input.fp16 = (entry[7] & 8) != 0;
        input.pack2 = (entry[7] & 0x10) != 0;
        input.row2 = entry[5];
        io.inputs.push_back(input);
    }
    const std::size_t size = detail::read_u32(data, kPsPatchSize);
    if (size > kPatchSlotDwords) detail::fail_malformed("implausible export patch size");
    for (std::size_t mrt = 0; mrt < kMrts; ++mrt) {
        const std::size_t at = kPsPatches + mrt * kPatchBytes;
        const std::uint32_t offset = detail::read_u32(data, at);
        if (offset == 0xFFFFFFFFu) continue;
        if ((static_cast<std::uint64_t>(offset) + size) * 4 > code_len)
            detail::fail_malformed("export patch outside the code");
        const std::size_t full = at + 4 + kPatchFull * kPatchSlotDwords * 4;
        ExportPatch patch;
        patch.mrt = static_cast<std::uint8_t>(mrt);
        patch.offset = offset;
        for (std::size_t i = 0; i < size; ++i) patch.code.push_back(detail::read_u32(data, full + 4 * i));
        io.patches.push_back(std::move(patch));
    }
    io.input_addr = detail::read_u32(data, kPsInputAddr);
    io.db_shader_control = detail::read_u32(data, kPsDbShaderControl);
    return io;
}

StageIo vs_io(std::span<const std::uint8_t> data) { return read_vs_io(data); }

VsIo read_vs_io(std::span<const std::uint8_t> data) {
    VsIo io;
    const auto input_bytes = typed(data, kVsInputs, 5, kVsInputCount);
    for (std::size_t i = 0; i < input_bytes.size(); i += 5) {
        const auto entry = input_bytes.subspan(i, 5);
        VsInput input;
        input.usage = usage_from_raw(entry[0]);
        input.row = entry[1];
        input.mask = static_cast<std::uint8_t>(entry[2] & 0xF);
        input.vgpr = entry[3];
        io.inputs.push_back(input);
    }
    const auto output_bytes = typed(data, kVsOutputs, 6, kVsOutputCount);
    for (std::size_t i = 0; i < output_bytes.size(); i += 6) {
        const auto entry = output_bytes.subspan(i, 6);
        VsOutput output;
        output.usage = usage_from_raw(entry[0]);
        output.row = entry[1];
        output.param = entry[2];
        output.fp16 = (entry[5] & 1) != 0;
        output.pack2 = (entry[5] & 2) != 0;
        io.outputs.push_back(output);
    }
    return io;
}

StageIo ms_io(std::span<const std::uint8_t> data, std::uint32_t lds_bytes) {
    MeshIo io;
    io.outputs = read_vs_io(data).outputs;
    const auto info = unwrap(detail::bytes(data, kMsInfo, 8), "stage data out of bounds");
    const auto half = [&](std::size_t at) {
        return static_cast<std::uint32_t>(info[at]) | static_cast<std::uint32_t>(info[at + 1]) << 8;
    };
    io.clip_cntl = detail::read_u32(data, kMsClipCntl);
    io.threads = half(0);
    io.max_vertices = half(2);
    io.max_primitives = half(4);
    io.topology = info[7];
    io.lds_bytes = lds_bytes;
    return io;
}

std::vector<NativeProgram> programs(std::span<const std::uint8_t> fssm) {
    if (!starts_with(fssm, "FSSM")) detail::fail_malformed("native blob lacks FSSM magic");
    if (detail::read_u32(fssm, 4) != kFssmVersion) detail::fail_malformed("unknown FSSM version");
    const auto payload = fssm.subspan(std::min(kFssmHeader, fssm.size()));
    if (detail::read_u64(fssm, 8) != payload.size()) detail::fail_malformed("FSSM payload size disagrees");
    if (detail::read_u64(payload, 0) != kGraphicsSlotCount) detail::fail_malformed("unexpected FSSM slot count");
    std::vector<NativeProgram> out;
    for (const Slot& slot : kSlots) {
        const std::uint64_t start = detail::read_u64(payload, slot.at);
        if (start == kAbsent) continue;
        const auto record = unwrap(detail::bytes(payload, start, kRecordSize), "FSSM record out of bounds");
        const auto code = unwrap(detail::bytes(payload, detail::read_u64(record, 0), detail::read_u64(record, 8)),
                                 "native code out of bounds");
        const std::uint16_t vgprs = static_cast<std::uint16_t>(detail::read_u32(record, kRecordRegs) & 0x3FF);
        if (vgprs > kMaxVgprs) detail::fail_malformed("implausible VGPR count");
        const auto table = [&](std::size_t at, std::size_t count_at) {
            const std::uint64_t len = kEntrySize * detail::read_u32(record, count_at);
            if (len == 0) return std::span<const std::uint8_t>{};
            return unwrap(detail::bytes(payload, detail::read_u64(record, at), len), "FSSM root table out of bounds");
        };
        NativeProgram program;
        const auto layout_bytes = table(kRecordLayout, kRecordCounts);
        for (std::size_t i = 0; i + kEntrySize <= layout_bytes.size(); i += kEntrySize) {
            const auto entry = layout_bytes.subspan(i, kEntrySize);
            RootParam param;
            param.root = root_type_from_raw(entry_u32(entry, 0));
            param.table_offset_dwords = entry_u32(entry, 4);
            param.sgprs = entry_u32(entry, 8);
            param.base_register = entry_u32(entry, 12);
            param.user_data = entry[16];
            param.slot = entry[17];
            program.layout.push_back(param);
        }
        const auto used_bytes = table(kRecordUsed, kRecordCounts + 4);
        for (std::size_t i = 0; i + kEntrySize <= used_bytes.size(); i += kEntrySize) {
            const auto entry = used_bytes.subspan(i, kEntrySize);
            UsedResource resource;
            resource.kind = resource_type_from_raw(entry[0]);
            resource.root = root_type_from_raw(entry[1]);
            resource.table_offset_dwords = entry_u32(entry, 4);
            resource.lower_bound = entry_u32(entry, 8);
            resource.stride = entry_u32(entry, 12);
            resource.dwords = entry[16];
            resource.user_data = entry[17];
            resource.slot = entry[18];
            program.used.push_back(resource);
        }
        const auto stage_data = payload.subspan(static_cast<std::size_t>(start + kRecordSize));
        switch (slot.stage) {
            case HwStage::Ps: program.io = ps_io(stage_data, code.size()); break;
            case HwStage::Vs: program.io = vs_io(stage_data); break;
            case HwStage::Cs: {
                ComputeIo io;
                io.rsrc2 = detail::read_u32(stage_data, kCsRsrc2);
                for (std::size_t i = 0; i < io.threads.size(); ++i)
                    io.threads[i] = detail::read_u32(stage_data, kCsThreads + 4 * i);
                program.io = io;
                break;
            }
            case HwStage::Ms: program.io = ms_io(stage_data, detail::read_u32(record, kRecordLds)); break;
            default: program.io = StageIo{}; break;
        }
        program.stage = slot.stage;
        program.wave64 = (detail::read_u32(record, kRecordFlags) & kFlagWave32) == 0;
        program.vgprs = vgprs;
        program.code.assign(code.begin(), code.end());
        out.push_back(std::move(program));
    }
    return out;
}

void store_u32(std::vector<std::uint8_t>& code, std::size_t at, std::uint32_t word) {
    code[at] = static_cast<std::uint8_t>(word);
    code[at + 1] = static_cast<std::uint8_t>(word >> 8);
    code[at + 2] = static_cast<std::uint8_t>(word >> 16);
    code[at + 3] = static_cast<std::uint8_t>(word >> 24);
}

}  // namespace

std::uint8_t user_sgpr(HwStage stage, std::uint8_t index) {
    switch (stage) {
        case HwStage::Ls:
        case HwStage::Hs:
        case HwStage::Es:
        case HwStage::Gs:
        case HwStage::Ms: return static_cast<std::uint8_t>(index + 8);
        case HwStage::Vs:
        case HwStage::Ps:
        case HwStage::Cs: return index;
    }
    return index;
}

Usage usage_from_raw(std::uint8_t raw) {
    Usage usage;
    if (raw == 6)
        usage.kind = Usage::Kind::Generic;
    else if (raw == 7)
        usage.kind = Usage::Kind::ClipDistance;
    else {
        usage.kind = Usage::Kind::Other;
        usage.raw = raw;
    }
    return usage;
}

RootType root_type_from_raw(std::uint32_t raw) {
    RootType type;
    switch (raw) {
        case 0: type.kind = RootType::Kind::Constants; break;
        case 1: type.kind = RootType::Kind::Descriptor; break;
        case 3: type.kind = RootType::Kind::Table; break;
        case 4: type.kind = RootType::Kind::ImmediateSampler; break;
        case 5: type.kind = RootType::Kind::ResourceHeap; break;
        case 6: type.kind = RootType::Kind::SamplerHeap; break;
        default:
            type.kind = RootType::Kind::Other;
            type.raw = raw;
            break;
    }
    return type;
}

ResourceType resource_type_from_raw(std::uint8_t raw) {
    ResourceType type;
    switch (raw) {
        case 0: type.kind = ResourceType::Kind::Srv; break;
        case 1: type.kind = ResourceType::Kind::Sampler; break;
        case 2: type.kind = ResourceType::Kind::Cbv; break;
        case 3: type.kind = ResourceType::Kind::Uav; break;
        default:
            type.kind = ResourceType::Kind::Other;
            type.raw = raw;
            break;
    }
    return type;
}

std::uint8_t ComputeIo::user_sgprs() const { return static_cast<std::uint8_t>((rsrc2 >> 1) & 0x1F); }

std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>> ComputeIo::system_sgprs() const {
    std::uint8_t next = user_sgprs();
    const auto take = [&](unsigned bit) -> std::optional<std::uint8_t> {
        if (((rsrc2 >> bit) & 1) == 0) return std::nullopt;
        next = static_cast<std::uint8_t>(next + 1);
        return static_cast<std::uint8_t>(next - 1);
    };
    std::array<std::optional<std::uint8_t>, 3> ids{take(7), take(8), take(9)};
    return {ids, take(10)};
}

std::uint8_t ComputeIo::initialized_sgprs() const {
    const auto [ids, info] = system_sgprs();
    std::uint8_t end = 0;
    for (const std::optional<std::uint8_t>& id : ids)
        if (id) end = std::max(end, static_cast<std::uint8_t>(*id + 1));
    if (info) end = std::max(end, static_cast<std::uint8_t>(*info + 1));
    return static_cast<std::uint8_t>(std::max(end, user_sgprs()) + (rsrc2 & 1));
}

std::uint8_t ComputeIo::thread_id_vgprs() const { return static_cast<std::uint8_t>((rsrc2 >> 11) & 3) + 1; }

std::uint32_t ComputeIo::lds_bytes() const { return ((rsrc2 >> 15) & 0x1FF) * 512; }

std::vector<std::uint8_t> NativeProgram::patched_code() const {
    std::vector<std::uint8_t> patched = code;
    if (const auto* ps = std::get_if<PsIo>(&io)) {
        for (const ExportPatch& patch : ps->patches) {
            const std::size_t at = static_cast<std::size_t>(patch.offset) * 4;
            for (std::size_t i = 0; i < patch.code.size(); ++i) {
                const std::size_t to = at + 4 * i;
                if (to > patched.size() || patched.size() - to < 4) continue;
                store_u32(patched, to, patch.code[i]);
            }
        }
    }
    return patched;
}

namespace {

std::span<const std::uint8_t> fspx_entry(std::span<const std::uint8_t> part, std::size_t count, std::uint32_t kind) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = kFspxHeader + kFspxEntry * i;
        if (detail::read_u32(part, at) != kind) continue;
        return unwrap(detail::bytes(part, detail::read_u32(part, at + 4), detail::read_u32(part, at + 8)),
                      "FSPX entry out of bounds");
    }
    detail::fail_malformed("FSPX entry missing");
}

NativeProgram fspx_program(std::span<const std::uint8_t> part) {
    if (detail::read_u32(part, 4) != kFspxVersion) detail::fail_malformed("unknown FSPX version");
    const std::size_t count = detail::read_u32(part, 8);
    if (count > kMaxXdatEntries) detail::fail_malformed("implausible FSPX entry count");
    const auto packet = fspx_entry(part, count, kFspxPacket);
    const auto code = fspx_entry(part, count, kFspxCode);
    const auto meta = fspx_entry(part, count, kFspxMetadata);
    if (static_cast<std::uint64_t>(detail::read_u32(meta, kFspxCodeDwords)) * 4 != code.size())
        detail::fail_malformed("FSPX code size disagrees");
    NativeProgram program;
    program.stage = HwStage::Cs;
    program.wave64 = (detail::read_u16(packet, kPacketFlags) & kPacketWave32) == 0;
    const std::uint32_t granules = (detail::read_u32(packet, kPacketRsrc1) & kVgprGranuleMask) + 1;
    program.vgprs = static_cast<std::uint16_t>(std::min(granules * (program.wave64 ? 4u : 8u), kMaxVgprs));
    for (std::size_t at = kFspxRecords; at + kFspxRecord <= meta.size(); at += kFspxRecord) {
        const std::span<const std::uint8_t> record = meta.subspan(at, kFspxRecord);
        if (record[0] == kFspxViewRecord) continue;
        if (record[0] == kFspxHashRecord) break;
        if (record[0] != kFspxResourceRecord) detail::fail_malformed("unknown FSPX metadata record");
        UsedResource used;
        used.kind = resource_type_from_raw(record[kFspxResourceType]);
        used.root = root_type_from_raw(record[kFspxRootType]);
        used.table_offset_dwords = detail::read_u32(record, kFspxTableOffset);
        used.lower_bound = detail::read_u32(record, kFspxRegister);
        used.dwords = record[kFspxDescriptorDwords];
        used.user_data = record[kFspxUserData];
        used.slot = record[kFspxRootSlot];
        std::uint32_t sgprs = 0;
        if (used.root.kind == RootType::Kind::Table)
            sgprs = 1;
        else if (used.root.kind == RootType::Kind::Descriptor)
            sgprs = 2;
        else if (used.root.kind != RootType::Kind::ImmediateSampler)
            detail::fail_malformed("an FSPX resource of an unknown root type");
        const bool known = std::any_of(program.layout.begin(), program.layout.end(),
                                       [&](const RootParam& param) { return param.slot == used.slot; });
        if (sgprs > 0 && !known) {
            RootParam param;
            param.root = used.root;
            param.sgprs = sgprs;
            param.base_register = used.lower_bound;
            param.user_data = used.user_data;
            param.slot = used.slot;
            program.layout.push_back(param);
        }
        program.used.push_back(used);
    }
    ComputeIo io;
    io.rsrc2 = detail::read_u32(packet, kPacketRsrc2);
    for (std::size_t axis = 0; axis < io.threads.size(); ++axis)
        io.threads[axis] = detail::read_u16(packet, kPacketThreads + 2 * axis);
    program.io = io;
    program.code.assign(code.begin(), code.end());
    return program;
}

}  // namespace

std::vector<NativeProgram> native_programs(std::span<const std::uint8_t> part) {
    if (starts_with(part, kFspxMagic)) return {fspx_program(part)};
    if (!starts_with(part.subspan(std::min(kXdatOffset, part.size())), "XDAT")) return {};
    const std::size_t count = detail::read_u32(part, kXdatOffset + 4);
    if (count > kMaxXdatEntries) detail::fail_malformed("implausible XDAT entry count");
    std::vector<NativeProgram> out;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = kXdatOffset + 12 + 12 * i;
        if (detail::read_u32(part, at) != kKindNative) continue;
        const std::uint64_t offset = detail::read_u32(part, at + 4);
        const std::uint64_t size = detail::read_u32(part, at + 8);
        const auto blob = unwrap(detail::bytes(part, offset, size), "XDAT entry out of bounds");
        const std::vector<std::uint8_t> unpacked = unpack(blob);
        std::vector<NativeProgram> found = programs(unpacked);
        out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return out;
}

}  // namespace g6shim::shader
