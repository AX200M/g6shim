// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/winrt/manifest.hpp>
#include <g6shim/winrt/package.hpp>

#include <string>

using namespace g6shim::winrt;

TEST(manifest_reads_element_attributes) {
    const std::string text =
        "<Package><Extensions><mx:XboxLive TitleId=\"0A1B2C3D\" PrimaryServiceConfigId=\"abc-123\" />"
        "</Extensions><Other TitleId=\"FFFFFFFF\"/></Package>";
    CHECK(manifest_attribute(text, "XboxLive", "TitleId") == "0A1B2C3D");
    CHECK(manifest_attribute(text, "XboxLive", "PrimaryServiceConfigId") == "abc-123");
    CHECK(manifest_attribute(text, "XboxLive", "Missing").empty());
    CHECK(manifest_attribute(text, "Absent", "TitleId").empty());
}

TEST(manifest_attribute_stays_inside_its_element) {
    CHECK(manifest_attribute("<a:XboxLive /><b TitleId=\"1\"/>", "XboxLive", "TitleId").empty());
    CHECK(manifest_attribute("<a:XboxLive TitleId=\"unterminated />", "XboxLive", "TitleId").empty());
    const std::string long_value(kMaxAttributeValue + 1, 'x');
    CHECK(manifest_attribute("<XboxLive TitleId=\"" + long_value + "\"/>", "XboxLive", "TitleId").empty());
}

TEST(manifest_in_process_servers) {
    const std::string text =
        "<Extension Category=\"windows.activatableClass.inProcessServer\"><InProcessServer>"
        "<Path>Services.dll</Path>"
        "<ActivatableClass ActivatableClassId=\"A.B.First\" ThreadingModel=\"both\" />"
        "<ActivatableClass ActivatableClassId=\"A.B.Second\" ThreadingModel=\"both\" />"
        "</InProcessServer></Extension>"
        "<InProcessServer><Path>..\\outside.dll</Path><ActivatableClass ActivatableClassId=\"A.Evil\"/></InProcessServer>"
        "<InProcessServer><Path>C:\\abs.dll</Path><ActivatableClass ActivatableClassId=\"A.Abs\"/></InProcessServer>"
        "<InProcessServer><ActivatableClass ActivatableClassId=\"A.NoPath\"/></InProcessServer>";
    const auto servers = parse_in_process_servers(text);
    CHECK(servers.size() == 2);
    CHECK(servers.count("A.B.First") && servers.at("A.B.First") == "Services.dll");
    CHECK(servers.count("A.B.Second") && servers.at("A.B.Second") == "Services.dll");
    CHECK(!servers.count("A.Evil") && !servers.count("A.Abs") && !servers.count("A.NoPath"));
}

TEST(manifest_in_process_servers_are_capped) {
    std::string text;
    for (std::size_t i = 0; i < kMaxInProcessServers + 5; ++i)
        text += "<InProcessServer><Path>x.dll</Path><ActivatableClass ActivatableClassId=\"C" + std::to_string(i) +
                "\"/></InProcessServer>";
    CHECK(parse_in_process_servers(text).size() == kMaxInProcessServers);
    CHECK(parse_in_process_servers("<InProcessServer><Path>x.dll</Path>").empty());
}

TEST(manifest_package_paths) {
    CHECK(is_safe_package_path("Services.dll"));
    CHECK(is_safe_package_path("sub\\Services.dll"));
    CHECK(!is_safe_package_path(""));
    CHECK(!is_safe_package_path("..\\x.dll"));
    CHECK(!is_safe_package_path("C:\\x.dll"));
    CHECK(!is_safe_package_path("\\x.dll"));
    CHECK(!is_safe_package_path("/x.dll"));
    CHECK(!is_safe_package_path(std::string(200, 'a')));
}
