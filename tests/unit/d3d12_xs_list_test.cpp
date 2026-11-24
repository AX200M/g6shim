// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/list.hpp"
#include "d3d12_xs/object.hpp"

#include <cstdint>
#include <cstring>

using namespace g6shim::d3d12_xs;

namespace {

std::uint64_t read_word(const Object* object, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(object) + offset, sizeof value);
    return value;
}

}  // namespace

TEST(d3d12_xs_every_inline_command_falls_back_to_the_table) {
    Object* allocator = nullptr;
    CHECK(list::create_allocator(nullptr, 0, &list::kAllocatorIids[0], &allocator) == S_OK);
    CHECK(allocator != nullptr);
    Object* commands = nullptr;
    CHECK(list::create_list(nullptr, 0, 0, allocator, nullptr, &list::kListIids[1], &commands) == S_OK);
    CHECK(commands != nullptr);
    if (commands == nullptr) return;
    CHECK(read_word(commands, 0x18) >= read_word(commands, 0x28));
    CHECK(read_word(commands, 0x18) >= read_word(commands, 0x30));
    Object* same = nullptr;
    CHECK(slot<HRESULT (*)(Object*, const Iid*, Object**)>(commands, 0)(commands, &list::kListIids[7], &same) ==
          S_OK);
    CHECK(same == commands);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 1);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x40)(commands) == 0);
    CHECK(slot<HRESULT (*)(Object*)>(commands, 0x48)(commands) == S_OK);
    void* upload = list::allocate_upload(commands, 40, 0x100);
    CHECK(upload != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(upload) % 0x100 == 0);
    std::memset(upload, 0xab, 40);
    CHECK(list::allocate_upload(commands, 8, 0x30) == nullptr);
    CHECK(list::upload_count(commands) == 1);
    CHECK(slot<HRESULT (*)(Object*, Object*, Object*)>(commands, 0x50)(commands, allocator, nullptr) == S_OK);
    CHECK(list::upload_count(commands) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands) == 0);
    CHECK(slot<std::uint32_t (*)(Object*)>(allocator, 0x10)(allocator) == 0);
}
