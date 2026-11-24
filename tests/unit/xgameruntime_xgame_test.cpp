// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xgame.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace xg = g6shim::xgameruntime::xgame;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;

using Check = bool (*)(Object*, std::uint32_t);
using Register = HResult (*)(Object*, const void*, void*, const void*, std::uint64_t*);
using Unregister = bool (*)(Object*, std::uint64_t, bool);

template <class Function>
Function at(const Object& object, std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(object, offset);
}

Object* self(const Object& object) { return const_cast<Object*>(&object); }

}  // namespace

TEST(xgameruntime_xgame_a_relaunch_keeps_our_options) {
    const std::vector<std::string> ours = {"run",       "G:/game.exe", "--execute", "--console",
                                           "series-s",  "--",          "-old"};
    const std::vector<std::string> again = xg::relaunch_args(ours, "G:/game.exe", "G:/game.exe", " -config=x  -y ");
    const std::vector<std::string> expected = {"run", "G:/game.exe", "--execute", "--console", "series-s", "--"};
    CHECK(again.size() == 8);
    if (again.size() == 8) {
        CHECK(std::equal(expected.begin(), expected.end(), again.begin()));
        CHECK(again[6] == "-config=x" && again[7] == "-y");
    }
    const std::vector<std::string> other = xg::relaunch_args(ours, "G:/game.exe", "G:/other.exe", "");
    CHECK(other.size() == 5);
    if (other.size() == 5) CHECK(other[1] == "G:/other.exe");
}

TEST(xgameruntime_xgame_served_areas_are_available_others_not) {
    const Check check = at<Check>(xg::kFeaturesObject, 0x18);
    CHECK(check(self(xg::kFeaturesObject), 18));
    CHECK(!check(self(xg::kFeaturesObject), 21));

    std::uint64_t token = 0;
    const Register register_event = at<Register>(xg::kInvitesObject, 0x18);
    CHECK(register_event(self(xg::kInvitesObject), nullptr, nullptr, nullptr, &token) ==
          g6shim::xgameruntime::kOk);
    CHECK(token != 0);
    CHECK(at<Unregister>(xg::kInvitesObject, 0x20)(self(xg::kInvitesObject), token, true));
}

TEST(xgameruntime_xgame_title_id_comes_from_the_manifest_element) {
    const auto id = xg::manifest_title_id(
        "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<Game configVersion=\"1\">"
        "<!-- <TitleId>00000000</TitleId> --><Identity Name=\"x\"/>"
        "<PersistentLocalStorage><SizeMB>1024</SizeMB></PersistentLocalStorage>"
        "<TitleId>639AA8DD</TitleId></Game>");
    CHECK(id.has_value());
    CHECK(id.value_or(0) == 0x639AA8DDu);
    CHECK(xg::manifest_title_id("<Game><TitleId>0a1b2c3d</TitleId></Game>").value_or(0) == 0x0a1b2c3du);
    CHECK(xg::manifest_title_id("<Game><TitleId>+1F</TitleId></Game>").value_or(0) == 0x1Fu);
    CHECK(!xg::manifest_title_id("<Game><TitleId>123456789</TitleId></Game>").has_value());
    CHECK(!xg::manifest_title_id("<Game><TitleId>xyz</TitleId></Game>").has_value());
    CHECK(!xg::manifest_title_id("<Game><Other>1</Other></Game>").has_value());
    CHECK(!xg::manifest_title_id(
                 "<Game><TitleId><!-- hidden -->1</TitleId></Game>")
               .has_value());
}
