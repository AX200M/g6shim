// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "shape_contexts.hpp"

#include <cstdint>
#include <cstring>

using namespace g6shim::acphal;

namespace {

struct Config {
    unsigned char bytes[kConfigBytes] = {};

    void set_count(std::size_t kind, std::uint32_t count) {
        std::memcpy(bytes + kConfigCountsOffset + 4 * kind, &count, 4);
    }

    void* array(std::size_t kind) const {
        void* p = nullptr;
        std::memcpy(&p, bytes + kConfigArraysOffset + sizeof(void*) * kind, sizeof(p));
        return p;
    }

    std::uint32_t apu(std::size_t kind) const {
        std::uint32_t a = 0;
        std::memcpy(&a, bytes + kConfigApuAddressesOffset + 4 * kind, 4);
        return a;
    }
};

}  // namespace

TEST(shape_config_reads_six_counts) {
    Config config;
    for (std::uint32_t kind = 0; kind < kShapeKinds; ++kind) config.set_count(kind, 10 + kind);
    const ShapeCounts counts = read_shape_counts(config.bytes);
    CHECK(counts[kShapeKindSrc] == 10);
    CHECK(counts[kShapeKindDma] == 13);
    CHECK(counts[kShapeKindXma] == 14);
    CHECK(counts[kShapeKindPcm] == 15);
}

TEST(shape_config_applies_the_console_limits) {
    ShapeCounts counts{};
    CHECK(shape_counts_valid(counts));
    counts[kShapeKindXma] = 0x200;
    CHECK(shape_counts_valid(counts));
    counts[kShapeKindXma] = 0x201;
    CHECK(!shape_counts_valid(counts));
    counts = {};
    counts[kShapeKindDma] = 0x1001;
    CHECK(!shape_counts_valid(counts));
    counts = {};
    counts[kShapeKindSrc] = 0x200;
    counts[kShapeKindXma] = 0x1FF;
    counts[kShapeKindPcm] = 1;
    CHECK(shape_counts_valid(counts));
    counts[kShapeKindSrc] = 0x1FF;
    CHECK(!shape_counts_valid(counts));
    counts = {};
    counts[kShapeKindPcm] = 2;
    CHECK(!shape_counts_valid(counts));
}

TEST(shape_contexts_fill_the_config_arrays) {
    ApuMemory memory;
    ShapeContexts contexts(memory);
    Config config;
    config.set_count(kShapeKindSrc, 0x200);
    config.set_count(kShapeKindDma, 0x400);
    config.set_count(kShapeKindXma, 0x200);
    CHECK(contexts.allocate(config.bytes) == S_OK);
    CHECK(config.array(kShapeKindEqComp) == nullptr);
    CHECK(config.array(kShapeKindPcm) == nullptr);
    CHECK(memory.contains(config.array(kShapeKindSrc), 0x200 * kSrcContextBytes));
    CHECK(memory.contains(config.array(kShapeKindDma), 0x400 * kDmaContextBytes));
    CHECK(memory.contains(config.array(kShapeKindXma), 0x200 * kXmaContextBytes));
    CHECK(config.apu(kShapeKindXma) == memory.apu_address(config.array(kShapeKindXma)));
    CHECK(config.apu(kShapeKindSrc) == memory.apu_address(config.array(kShapeKindSrc)));
    CHECK(config.apu(kShapeKindPcm) == 0);
    contexts.with_table([&](const ShapeTable& table) {
        CHECK(table.context(kShapeKindXma, 0) == config.array(kShapeKindXma));
        CHECK(table.context(kShapeKindXma, 1) ==
              static_cast<unsigned char*>(config.array(kShapeKindXma)) + kXmaContextBytes);
        CHECK(table.context(kShapeKindXma, 0x200) == nullptr);
        CHECK(table.context(kShapeKindEqComp, 0) == nullptr);
    });
}

TEST(shape_contexts_place_pcm_after_xma) {
    ApuMemory memory;
    ShapeContexts contexts(memory);
    Config config;
    config.set_count(kShapeKindSrc, 9);
    config.set_count(kShapeKindEqComp, 3);
    config.set_count(kShapeKindFilterVolume, 4);
    config.set_count(kShapeKindXma, 8);
    config.set_count(kShapeKindPcm, 1);
    CHECK(contexts.allocate(config.bytes) == S_OK);
    auto* xma = static_cast<unsigned char*>(config.array(kShapeKindXma));
    CHECK(config.array(kShapeKindPcm) == xma + 8 * kXmaContextBytes);
    CHECK(config.apu(kShapeKindPcm) == config.apu(kShapeKindXma) + 8 * kXmaContextBytes);
    CHECK(memory.contains(config.array(kShapeKindPcm), kPcmContextBytes));
    CHECK(memory.contains(config.array(kShapeKindEqComp), 3 * kEqCompContextBytes));
    CHECK(memory.contains(config.array(kShapeKindFilterVolume), 4 * kFilterVolumeContextBytes));
    CHECK(contexts.release() == S_OK);
    CHECK(memory.block_count() == 0);
}

TEST(shape_contexts_are_allocated_once) {
    ApuMemory memory;
    ShapeContexts contexts(memory);
    Config config;
    config.set_count(kShapeKindXma, 4);
    CHECK(contexts.release() == kShapeContextsNotAllocated);
    CHECK(contexts.allocate(config.bytes) == S_OK);
    CHECK(contexts.allocate(config.bytes) == kShapeContextsInUse);
    CHECK(memory.block_count() == 1);
    CHECK(contexts.release() == S_OK);
    CHECK(memory.block_count() == 0);
    CHECK(contexts.allocate(config.bytes) == S_OK);
}

TEST(shape_contexts_reject_bad_configs) {
    ApuMemory memory;
    ShapeContexts contexts(memory);
    Config config;
    config.set_count(kShapeKindXma, kMaxContexts[kShapeKindXma] + 1);
    CHECK(contexts.allocate(config.bytes) == E_INVALIDARG);
    CHECK(contexts.allocate(nullptr) == E_INVALIDARG);
    CHECK(memory.block_count() == 0);
}
