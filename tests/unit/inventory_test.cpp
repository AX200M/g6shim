// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"
#include "sample_pe.hpp"

#include "g6shim/inventory.hpp"

namespace {

g6shim::ModuleUse windows_module(const std::string& dll, const std::vector<g6shim::pe::Import>& imports) {
    g6shim::ModuleUse m;
    m.dll = dll;
    m.windows = true;
    for (const auto& i : imports) m.functions[i.display()].import = i;
    return m;
}

g6shim::pe::Import named(const std::string& name) {
    g6shim::pe::Import i;
    i.name = name;
    return i;
}

g6shim::pe::Import by_ordinal(std::uint16_t ordinal) {
    g6shim::pe::Import i;
    i.ordinal = ordinal;
    return i;
}

}  // namespace

TEST(inventory_skips_console_os_image) {
    using g6shim::test::write_file;
    g6shim::test::TempDir dump;
    const auto pe = g6shim::test::build_sample_pe();
    write_file(dump.path / "Mount" / "game.dll", pe);
    write_file(dump.path / "EmbeddedXvd" / "Windows" / "System32" / "fakeconsole.dll", pe);

    const auto whole = g6shim::scan_paths({dump.path});
    CHECK(whole.files.size() == 1 && whole.files[0].path.filename() == "game.dll");
    CHECK(whole.failures.size() == 1 && whole.failures[0].path.filename() == "EmbeddedXvd");
    CHECK(whole.modules.count("fakeconsole.dll") && !whole.modules.at("fakeconsole.dll").local);

    const auto os = g6shim::scan_paths({dump.path / "EmbeddedXvd"});
    CHECK(os.files.size() == 1 && os.failures.empty());

    const auto both = g6shim::scan_paths({dump.path / "Mount", dump.path / "EmbeddedXvd"});
    CHECK(both.modules.count("fakeconsole.dll") && both.modules.at("fakeconsole.dll").local);
}

TEST(inventory_checks_windows_dlls) {
    using g6shim::test::write_file;
    g6shim::test::TempDir system;
    write_file(system.path / "test.dll", g6shim::test::build_sample_pe());
    write_file(system.path / "junk.dll", std::vector<std::uint8_t>(64, 0x41));

    g6shim::Inventory inv;
    inv.modules["test.dll"] =
        windows_module("TEST.dll", {named("Alpha"), named("Gamma"), named("Beta"), by_ordinal(2), by_ordinal(9)});
    inv.modules["absent.dll"] = windows_module("absent.dll", {named("X")});
    inv.modules["api-ms-win-core-test-l1-1-0.dll"] = windows_module("api-ms-win-core-test-l1-1-0.dll", {named("Y")});
    inv.modules["junk.dll"] = windows_module("junk.dll", {named("Z")});
    inv.modules["console.dll"] = windows_module("console.dll", {named("W")});
    inv.modules["console.dll"].windows = false;

    g6shim::check_windows_dlls(inv, system.path, {"test.dll"});

    const auto& t = inv.modules.at("test.dll");
    CHECK(t.pc == g6shim::PcDll::Found && t.known_dll);
    CHECK(!t.functions.at("Alpha").missing_on_pc);
    CHECK(!t.functions.at("Gamma").missing_on_pc);
    CHECK(t.functions.at("Beta").missing_on_pc);
    CHECK(!t.functions.at("#2").missing_on_pc);
    CHECK(t.functions.at("#9").missing_on_pc);

    const auto& absent = inv.modules.at("absent.dll");
    CHECK(absent.pc == g6shim::PcDll::NotFound && !absent.known_dll && absent.functions.at("X").missing_on_pc);
    CHECK(inv.modules.at("api-ms-win-core-test-l1-1-0.dll").pc == g6shim::PcDll::NotChecked);
    CHECK(inv.modules.at("junk.dll").pc == g6shim::PcDll::NotChecked);
    CHECK(inv.modules.at("console.dll").pc == g6shim::PcDll::NotChecked);
    CHECK(!inv.modules.at("console.dll").functions.at("W").missing_on_pc);
}

TEST(inventory_known_dlls) {
    const auto known = g6shim::known_dlls();
    CHECK(known.count("ntdll.dll") && known.count("kernel32.dll"));
    CHECK(!g6shim::system_dir().empty());
}
