// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "g6shim/hle.hpp"

using g6shim::hle::DefFile;
using g6shim::hle::next_stub_id;

TEST(def_parses_exports) {
    const char* text =
        "LIBRARY \"FakeConsole.dll\"\n"
        "EXPORTS\n"
        "    Foo=g6shim_stub_0\n"
        "    Bar = g6shim_Bar   ; a trailing remark\n"
        "    g6shim_stub_1 @7 NONAME\n"
        "    g6shim_Secret @ 9 NONAME\n"
        "    Baz\n"
        "    Qux DATA\n";
    const auto def = DefFile::parse(text, "fallback.dll");
    CHECK(def.dll == "FakeConsole.dll");
    CHECK(def.exports.size() == 6);
    if (def.exports.size() == 6) {
        CHECK(def.exports[0].key() == "Foo" && def.exports[0].is_stub());
        CHECK(def.exports[1].key() == "Bar" && def.exports[1].internal == "g6shim_Bar" && !def.exports[1].is_stub());
        CHECK(def.exports[2].key() == "#7" && def.exports[2].is_stub());
        CHECK(def.exports[3].key() == "#9" && !def.exports[3].is_stub());
        CHECK(def.exports[4].key() == "Baz" && !def.exports[4].is_stub());
        CHECK(def.exports[5].data);
    }
}

TEST(def_library_name_and_fallback) {
    const auto lib = DefFile::parse("LIBRARY \"other.dll\"\nEXPORTS\n  A\n", "fallback.dll");
    CHECK(lib.dll == "other.dll" && lib.exports.size() == 1);
    const auto bare = DefFile::parse("EXPORTS\n  A\n", "fallback.dll");
    CHECK(bare.dll == "fallback.dll");
    const auto forwarded = DefFile::parse("EXPORTS\n  A=kernel32.A\n", "x.dll");
    CHECK(forwarded.exports.size() == 1 && !forwarded.exports[0].is_stub());
}

TEST(def_safe_names) {
    CHECK(g6shim::hle::is_def_safe_name("Foo"));
    CHECK(g6shim::hle::is_def_safe_name("?Mangled@@YAXXZ"));
    CHECK(!g6shim::hle::is_def_safe_name(""));
    CHECK(!g6shim::hle::is_def_safe_name("has space"));
    CHECK(!g6shim::hle::is_def_safe_name("a=b"));
    CHECK(!g6shim::hle::is_def_safe_name("@leading"));
}

TEST(def_next_stub_id) {
    const auto def = DefFile::parse(
        "EXPORTS\n  A=g6shim_stub_0\n  B=g6shim_stub_4\n  C=g6shim_C\n  g6shim_stub_2 @7 NONAME\n", "x.dll");
    CHECK(next_stub_id(def, "") == 5);
    CHECK(next_stub_id(def, "G6SHIM_STUB(0, \"x.dll\", \"A\")\nG6SHIM_STUB(12, \"x.dll\", \"Gone\")\n") == 13);
    CHECK(next_stub_id(def, "G6SHIM_STUB( 9, \"x.dll\", \"Spaced\")") == 10);
    CHECK(next_stub_id(def, "G6SHIM_STUB(abc)\nG6SHIM_STUB(99999999999999999999)\nG6SHIM_STUB(4294967295)\nG6SHIM_STUB(") == 5);
    CHECK(next_stub_id(DefFile::parse("EXPORTS\n  A=g6shim_stub_x\n", "x.dll"), "") == 0);
}
