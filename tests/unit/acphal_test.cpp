// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "acp.hpp"
#include "shape_config.hpp"
#include "xma.hpp"
#include "xma_voices.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" HRESULT WINAPI g6shim_AcpHalAllocateShapeContexts(void* config);
extern "C" HRESULT WINAPI g6shim_AcpHalReleaseShapeContexts();
extern "C" HRESULT WINAPI g6shim_AcpHalCreate(void** hal);
extern "C" HRESULT WINAPI g6shim_ApuAlloc(void** va, UINT32* apu, UINT32 size, UINT32 alignment, UINT32 flags);
extern "C" void WINAPI g6shim_ApuFree(void* va);
extern "C" UINT32 WINAPI g6shim_ApuMapVirtualAddress(const void* va);
extern "C" BOOLEAN WINAPI g6shim_ApuIsVirtualAddressValid(const void* va, UINT32 size);
extern "C" void* WINAPI g6shim_ApuMapApuAddress(UINT32 apu);

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
};

struct Context {
    std::uint32_t* words;

    std::uint32_t field(std::size_t word, std::uint32_t shift, std::uint32_t bits) const {
        return (*reinterpret_cast<volatile std::uint32_t*>(words + word) >> shift) & ((1u << bits) - 1);
    }

    void set(std::size_t word, std::uint32_t value) const {
        *reinterpret_cast<volatile std::uint32_t*>(words + word) = value;
    }
};

void put_bits(std::vector<std::uint8_t>& data, std::size_t pos, std::uint32_t count, std::uint32_t value) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = pos + i;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (7 - at % 8));
        if (((value >> (count - 1 - i)) & 1u) != 0)
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] | mask);
        else
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] & ~mask);
    }
}

std::vector<std::uint8_t> silent_stream() {
    std::vector<std::uint8_t> data(2 * xma::kPacketBytes, 0xff);
    for (std::uint32_t packet = 0; packet < 2; ++packet) {
        const std::size_t base = static_cast<std::size_t>(packet) * xma::kPacketBytes * 8;
        put_bits(data, base, 32, 3u << 26);
        for (std::uint32_t frame = 0; frame < 3; ++frame) {
            const std::size_t at = base + 32 + frame * 31;
            put_bits(data, at, 15, 31);
            put_bits(data, at + 15, 2, 0b10);
            put_bits(data, at + 17, 8 + 1 + 3 + 2, 0);
        }
    }
    return data;
}

}  // namespace

TEST(acphal_heap_maps_every_byte_of_a_block) {
    void* va = nullptr;
    std::uint32_t apu = 0;
    CHECK(g6shim_ApuAlloc(&va, &apu, 0x100, 128, 1) == S_OK);
    CHECK(apu != 0 && apu % 128 == 0);
    auto* inner = static_cast<unsigned char*>(va) + 0x40;
    CHECK(g6shim_ApuMapVirtualAddress(inner) == apu + 0x40);
    CHECK(g6shim_ApuIsVirtualAddressValid(va, 128));
    CHECK(g6shim_ApuMapApuAddress(apu + 4) == static_cast<unsigned char*>(va) + 4);
    g6shim_ApuFree(va);
    CHECK(g6shim_ApuMapVirtualAddress(va) == 0);
    CHECK(g6shim_ApuMapApuAddress(apu) == nullptr);
    CHECK(g6shim_ApuAlloc(&va, &apu, 0x100, 6, 1) == E_INVALIDARG);
}

TEST(acphal_apu_map_apu_address_maps_inside_blocks_only) {
    CHECK(g6shim_ApuMapApuAddress(0) == nullptr);
    void* va = nullptr;
    std::uint32_t apu = 0;
    CHECK(g6shim_ApuAlloc(&va, &apu, 0x800, 0, 1) == S_OK);
    CHECK(g6shim_ApuMapApuAddress(apu) == va);
    CHECK(g6shim_ApuMapApuAddress(apu + 0x7FF) == static_cast<unsigned char*>(va) + 0x7FF);
    CHECK(g6shim_ApuMapApuAddress(apu + 0x800) == nullptr);
    g6shim_ApuFree(va);
    CHECK(g6shim_ApuMapApuAddress(apu) == nullptr);
}

TEST(acphal_xma_contexts_decode_into_their_output_rings) {
    Config config;
    config.set_count(kShapeKindXma, 4);
    CHECK(g6shim_AcpHalAllocateShapeContexts(config.bytes) == S_OK);
    auto* array = static_cast<std::uint8_t*>(config.array(kShapeKindXma));
    CHECK(array != nullptr);
    Context context{reinterpret_cast<std::uint32_t*>(array + kXmaContextBytes)};

    void* input = nullptr;
    std::uint32_t input_apu = 0;
    CHECK(g6shim_ApuAlloc(&input, &input_apu, 0x1000, 0x800, 1) == S_OK);
    const std::vector<std::uint8_t> stream = silent_stream();
    std::memcpy(input, stream.data(), stream.size());
    void* output = nullptr;
    std::uint32_t output_apu = 0;
    CHECK(g6shim_ApuAlloc(&output, &output_apu, 0x400, 0x100, 2) == S_OK);
    std::memset(output, 0x55, 0x400);

    context.set(0, 2 | 1u << 20 | 4u << 22);
    context.set(1, 1u << 31 | 3u << 27);
    context.set(2, 32);
    context.set(5, input_apu);
    context.set(7, output_apu);

    void* acp = nullptr;
    CHECK(g6shim_AcpHalCreate(&acp) == S_OK);
    const AcpMethods* methods = static_cast<Acp*>(acp)->methods;
    CHECK((methods->connect)(static_cast<Acp*>(acp), 0x1000, 0x1000) == S_OK);
    const auto submit = [&](std::uint32_t command, const std::vector<std::uint32_t>& words) {
        CHECK((methods->submit)(static_cast<Acp*>(acp), command, 0xf1, UINT32_MAX, words.data(), 0) == S_OK);
    };
    submit(5, {1});
    xma_pump();
    CHECK(context.field(0, 27, 5) == 0);
    CHECK(context.field(1, 31, 1) == 0);
    const auto* ring = static_cast<const std::uint8_t*>(output);
    bool silent = true;
    for (std::size_t i = 0; i < 0x400; ++i)
        if (ring[i] != 0) silent = false;
    CHECK(silent);

    for (int i = 0; i < 20; ++i) {
        submit(18, {1, 4});
        xma_pump();
        if (context.field(2, 31, 1) == 1 && context.field(1, 31, 1) == 1) break;
    }
    const auto produced = xma_produced(1);
    CHECK(produced.has_value() && *produced == 26 * xma::kSubframeSamples);
    CHECK(context.field(0, 20, 2) == 0);
    CHECK(context.field(2, 26, 5) == 4);

    context.set(9, 3);
    submit(18, {1, UINT32_MAX});
    CHECK(context.field(9, 0, 5) == 2);

    submit(7, {1});
    CHECK(!xma_produced(1).has_value());
    (methods->release)(static_cast<Acp*>(acp));
    g6shim_ApuFree(input);
    g6shim_ApuFree(output);
    CHECK(g6shim_AcpHalReleaseShapeContexts() == S_OK);
}

TEST(acphal_full_message_queues_count_drops) {
    void* acp = nullptr;
    CHECK(g6shim_AcpHalCreate(&acp) == S_OK);
    const AcpMethods* methods = static_cast<Acp*>(acp)->methods;
    CHECK((methods->connect)(static_cast<Acp*>(acp), 16, 2) == S_OK);
    const std::uint32_t mask = 0x10;
    CHECK((methods->submit)(static_cast<Acp*>(acp), 2, 0, UINT32_MAX, &mask, 0) == S_OK);
    for (std::uint64_t id = 1; id <= 4; ++id)
        CHECK((methods->submit)(static_cast<Acp*>(acp), 12, id, UINT32_MAX, nullptr, 0) == S_OK);
    CHECK((methods->message_count)(static_cast<Acp*>(acp)) == 2);
    Message message{};
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    CHECK((methods->submit)(static_cast<Acp*>(acp), 12, 5, UINT32_MAX, nullptr, 0) == S_OK);
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    CHECK(message.dropped == 2);
    (methods->release)(static_cast<Acp*>(acp));
}

TEST(acphal_startup_connects_and_registers_messages) {
    CHECK(g6shim_AcpHalCreate(nullptr) == E_INVALIDARG);
    Config config;
    config.set_count(kShapeKindSrc, 0x200);
    config.set_count(kShapeKindDma, 0x400);
    config.set_count(kShapeKindXma, 0x200);
    CHECK(sizeof(config.bytes) == 0x60);
    CHECK(g6shim_AcpHalAllocateShapeContexts(config.bytes) == S_OK);
    CHECK(config.array(kShapeKindSrc) != nullptr);
    CHECK(config.array(kShapeKindDma) != nullptr);
    CHECK(config.array(kShapeKindXma) != nullptr);
    CHECK(config.array(kShapeKindEqComp) == nullptr);
    std::memset(config.array(kShapeKindSrc), 0, 0x200u << 5);

    void* acp = nullptr;
    CHECK(g6shim_AcpHalCreate(&acp) == S_OK);
    const AcpMethods* methods = static_cast<Acp*>(acp)->methods;
    CHECK((methods->connect)(static_cast<Acp*>(acp), 0x804, 0x804) == S_OK);
    const std::uint32_t mask = 0xfe;
    CHECK((methods->submit)(static_cast<Acp*>(acp), 2, 1, UINT32_MAX, &mask, 0) == S_OK);
    Message message{};
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    CHECK(message.kind == 0x10 && message.dropped == 0);
    std::uint64_t id = 0;
    std::memcpy(&id, message.data + 4, sizeof(id));
    CHECK(id == 1);
    CHECK(!(methods->pop_message)(static_cast<Acp*>(acp), &message));
    CHECK((methods->disconnect)(static_cast<Acp*>(acp)) == S_OK);
    CHECK((methods->message_count)(static_cast<Acp*>(acp)) == 1);
    (methods->release)(static_cast<Acp*>(acp));
    CHECK(g6shim_AcpHalReleaseShapeContexts() == S_OK);
}

TEST(acphal_shape_contexts_hand_their_xma_array_to_the_decoder) {
    Config config;
    config.set_count(kShapeKindXma, 4);
    CHECK(g6shim_AcpHalAllocateShapeContexts(config.bytes) == S_OK);
    CHECK(xma_voice_count() == 4);
    CHECK(g6shim_AcpHalReleaseShapeContexts() == S_OK);
    CHECK(xma_voice_count() == 0);
    CHECK(g6shim_AcpHalReleaseShapeContexts() == kShapeContextsNotAllocated);
    CHECK(xma_voice_count() == 0);
}

TEST(acphal_commands_complete_and_notify) {
    void* acp = nullptr;
    CHECK(g6shim_AcpHalCreate(&acp) == S_OK);
    const AcpMethods* methods = static_cast<Acp*>(acp)->methods;
    CHECK((methods->disconnect)(static_cast<Acp*>(acp)) == E_FAIL);
    CHECK((methods->connect)(static_cast<Acp*>(acp), 16, 16) == S_OK);
    CHECK((methods->connect)(static_cast<Acp*>(acp), 0, 16) == E_INVALIDARG);
    const std::uint32_t mask = 0x02 | 0x10;
    CHECK((methods->submit)(static_cast<Acp*>(acp), 2, 0, UINT32_MAX, &mask, 0) == S_OK);

    void* word = nullptr;
    std::uint32_t word_apu = 0;
    CHECK(g6shim_ApuAlloc(&word, &word_apu, 4, 4, 1) == S_OK);
    *static_cast<std::uint32_t*>(word) = 0;
    const std::uint32_t words[2] = {0x1234, 0x99};
    CHECK((methods->submit)(static_cast<Acp*>(acp), 1, 7, UINT32_MAX, words, word_apu) == S_OK);
    CHECK(*reinterpret_cast<volatile std::uint32_t*>(word) == 1);
    CHECK((methods->message_count)(static_cast<Acp*>(acp)) == 2);

    Message message{};
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    std::uint32_t flowgraph = 0;
    std::memcpy(&flowgraph, message.data, sizeof(flowgraph));
    CHECK(message.kind == 0x02 && flowgraph == 0x99);
    CHECK((methods->pop_message)(static_cast<Acp*>(acp), &message));
    std::uint32_t command = 0;
    std::uint64_t id = 0;
    std::memcpy(&command, message.data, sizeof(command));
    std::memcpy(&id, message.data + 4, sizeof(id));
    CHECK(message.kind == 0x10 && command == 1 && id == 7);
    CHECK(!(methods->pop_message)(static_cast<Acp*>(acp), &message));

    (methods->release)(static_cast<Acp*>(acp));
    g6shim_ApuFree(word);
}
