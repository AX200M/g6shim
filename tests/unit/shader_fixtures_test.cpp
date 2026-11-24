// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <vector>

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;
using namespace g6shim::test::lowering;

struct Fixture {
    const char* name;
    std::vector<std::uint32_t> words;
};

const Fixture kFixtures[] = {
#include "gfx10_fixtures.inc"
};

constexpr std::uint16_t kFixtureVgprs = 8;
constexpr std::uint8_t kConstantsUserData = 0;
constexpr std::uint8_t kTableUserData = 4;
constexpr std::uint8_t kConstantsSlot = 0;
constexpr std::uint8_t kTableSlot = 1;
constexpr std::uint32_t kUserSgprs = 5;
constexpr std::uint32_t kUserSgprShift = 1;

Lowered lower_fixture(const Fixture& fixture) {
    return lower_compute(fixture.words, kFixtureVgprs,
                         {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
                          root_param(RootType::Kind::Table, kTableUserData, kTableSlot)},
                         {used_uav(kTableUserData, kTableSlot)}, kUserSgprs << kUserSgprShift, {1, 1, 1});
}

}  // namespace

TEST(shader_fixtures_translate_and_validate) {
    CHECK(std::size(kFixtures) > 0);
    for (const Fixture& fixture : kFixtures) {
        bool translated = true;
        Lowered lowered;
        try {
            lowered = lower_fixture(fixture);
        } catch (const std::exception& error) {
            std::printf("fixture %s did not translate: %s\n", fixture.name, error.what());
            translated = false;
        }
        CHECK(translated);
        if (!translated) continue;
        CHECK(lowered.user_data.size() == kUserSgprs);
        check_dxil(lowered.dxil, fixture.name);
    }
}
