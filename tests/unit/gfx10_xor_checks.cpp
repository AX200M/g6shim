// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

void gfx10_xor_addresses_match_the_reference_body();
void gfx10_xor_and_multisample_copies_round_trip_body();
void gfx10_unpublished_xor_equations_stay_unaddressable_body();

TEST(gfx10_xor_addresses_match_the_reference) { gfx10_xor_addresses_match_the_reference_body(); }

TEST(gfx10_xor_and_multisample_copies_round_trip) { gfx10_xor_and_multisample_copies_round_trip_body(); }

TEST(gfx10_unpublished_xor_equations_stay_unaddressable) {
    gfx10_unpublished_xor_equations_stay_unaddressable_body();
}
