// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xasync.hpp"
#include "xuser.hpp"

#include <cstdint>
#include <cstring>

namespace {

namespace xa = g6shim::xgameruntime::xasync;
namespace xu = g6shim::xgameruntime::xuser;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;
using Handle = const void*;

using Gamertag = HResult (*)(Object*, Handle, std::uint32_t, std::size_t, std::uint8_t*, std::size_t*);
using Add = HResult (*)(Object*, std::uint32_t, xa::AsyncBlock*);
using AddResult = HResult (*)(Object*, xa::AsyncBlock*, Handle*);
using GetId = HResult (*)(Object*, Handle, std::uint64_t*);
using Age = HResult (*)(Object*, Handle, std::uint32_t*);
using Token = HResult (*)(Object*, Handle, std::uint32_t, const char*, const char*, std::size_t, const void*,
                          std::size_t, const void*, xa::AsyncBlock*);

template <class Function>
Function at(const Object& object, std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(object, offset);
}

Object* self() { return const_cast<Object*>(&xu::kObject); }

}  // namespace

TEST(xgameruntime_xuser_a_gamertag_without_a_suffix_has_an_empty_modern_suffix) {
    struct Slot {
        const Object* object;
        std::size_t offset;
    };
    const Slot slots[] = {{&xu::kGamertagObject, 0x18}, {&xu::kObject, 0x78}};
    const char* expected[] = {"Player", "Player", "", "Player"};
    for (const Slot& slot : slots) {
        const Gamertag get = at<Gamertag>(*slot.object, slot.offset);
        for (std::uint32_t component = 0; component < 4; ++component) {
            std::uint8_t name[16];
            std::memset(name, 0xff, sizeof name);
            std::size_t used = 0;
            CHECK(get(self(), nullptr, component, sizeof name, name, &used) == g6shim::xgameruntime::kOk);
            const std::size_t length = std::strlen(expected[component]) + 1;
            CHECK(used == length);
            CHECK(std::memcmp(name, expected[component], length) == 0);
        }
    }
}

TEST(xgameruntime_xuser_startup_adds_the_user_and_reads_its_id) {
    xa::AsyncBlock block{};
    CHECK(at<Add>(xu::kObject, 0x38)(self(), 1, &block) == g6shim::xgameruntime::kOk);
    CHECK(xa::XAsyncGetStatus(&block, true) == g6shim::xgameruntime::kOk);
    Handle handle = nullptr;
    CHECK(at<AddResult>(xu::kObject, 0x40)(self(), &block, &handle) == g6shim::xgameruntime::kOk);
    CHECK(handle != nullptr);
    std::uint64_t xuid = 0;
    CHECK(at<GetId>(xu::kObject, 0x58)(self(), handle, &xuid) == g6shim::xgameruntime::kOk);
    CHECK(xuid == xu::kXuid);

    std::uint8_t name[16] = {};
    std::size_t used = 0;
    const Gamertag get = at<Gamertag>(xu::kGamertagObject, 0x18);
    CHECK(get(self(), handle, 0, sizeof name, name, &used) == g6shim::xgameruntime::kOk);
    CHECK(used == 7 && std::memcmp(name, "Player", 7) == 0);
    CHECK(get(self(), handle, 0, 2, name, &used) == g6shim::xgameruntime::kNotSufficientBuffer);

    std::uint32_t age = 0;
    CHECK(at<Age>(xu::kObject, 0x98)(self(), handle, &age) == g6shim::xgameruntime::kOk);
    CHECK(age == 3);

    const Token token = at<Token>(xu::kObject, 0xb8);
    CHECK(token(self(), handle, 0, "GET", "https://example.invalid/", 0, nullptr, 0, nullptr, &block) ==
          g6shim::xgameruntime::kOk);
    CHECK(xa::XAsyncGetStatus(&block, true) == xu::kFailedToGetToken);
}
