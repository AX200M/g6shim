// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/object.hpp"

TEST(d3d12_xs_guids_round_trip_through_text) {
    const g6shim::d3d12_xs::Iid id =
        g6shim::d3d12_xs::guid(0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7});
    CHECK(id[0] == 0xf1);
    CHECK(id[1] == 0x19);
    CHECK(id[2] == 0x98);
    CHECK(id[3] == 0x18);
    CHECK(g6shim::d3d12_xs::guid_text(id) == "189819f1-1db6-4b57-be54-1821339b85f7");
}
