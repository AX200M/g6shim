// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "graphics_object.hpp"

#include <cstdint>
#include <cstring>

using namespace g6shim::d3d12;

namespace {

struct FakePc;
using Method = std::uintptr_t (*)(FakePc*, std::uintptr_t);

struct FakePc {
    const Method* table;
    std::uintptr_t last = 0;
};

std::uintptr_t fake_slot0(FakePc* self, std::uintptr_t a) {
    self->last = a;
    return 100 + a;
}
std::uintptr_t fake_slot1(FakePc* self, std::uintptr_t a) {
    self->last = a;
    return 200 + a;
}
std::uintptr_t fake_slot2(FakePc* self, std::uintptr_t a) {
    self->last = a;
    return 300 + a;
}
const Method kFakeTable[] = {&fake_slot0, &fake_slot1, &fake_slot2};

constexpr std::size_t kConsoleBytes = 0x58;

struct Wrapper {
    GraphicsHead head;
    unsigned char console[kConsoleBytes - sizeof(GraphicsHead)];
    FakePc* pc;
};
static_assert(offsetof(Wrapper, pc) == kConsoleBytes);

std::uintptr_t override_slot1(Wrapper*, std::uintptr_t a) { return 900 + a; }

using Call = std::uintptr_t (*)(void*, std::uintptr_t);
using Probe = HRESULT (*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

}  // namespace

TEST(graphics_head_matches_the_console_layout) {
    GraphicsHead head;
    init_head(head, nullptr);
    CHECK(head.refs.load() == 1);
    CHECK(head.device_bits == 0);
}

TEST(graphics_tables_forward_override_and_probe) {
    FakePc pc{kFakeTable};
    Wrapper w{};
    w.pc = &pc;
    const TableSpec spec{"test object", offsetof(Wrapper, pc), 3, 6};
    const void* const* table = build_table(spec, {{1, reinterpret_cast<const void*>(&override_slot1)}});
    CHECK(table != nullptr);
    init_head(w.head, table);
    CHECK(reinterpret_cast<Call>(table[0])(&w, 5) == 105);
    CHECK(pc.last == 5);
    CHECK(reinterpret_cast<Call>(table[1])(&w, 5) == 905);
    CHECK(reinterpret_cast<Call>(table[2])(&w, 7) == 307);
    CHECK(pc.last == 7);
    CHECK(reinterpret_cast<Probe>(table[4])(&w, 1, 2, 3, 4) == E_NOTIMPL);
    CHECK(std::strcmp(table_kind(&w), "test object") == 0);
}

TEST(graphics_tables_reject_bad_specs) {
    CHECK(build_table({"too long", 0x10, 0, kMaxTableSlots + 1}, {}) == nullptr);
    CHECK(build_table({"prefix too long", 0x10, 5, 4}, {}) == nullptr);
    CHECK(forward_thunk(kMaxPcObjectOffset + 8, 0) == nullptr);
    CHECK(forward_thunk(0x18, kMaxTableSlots) == nullptr);
    CHECK(forward_thunk(0x18, 3) == forward_thunk(0x18, 3));
    CHECK(forward_thunk(0x18, 3) != forward_thunk(0x360, 3));
}
