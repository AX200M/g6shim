// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "g6shim/hle.hpp"
#include "g6shim/inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using g6shim::hle::Status;

namespace {

void usage() {
    std::cerr <<
        "usage: g6shim-scan <game folder or .exe/.dll files...> [options]\n"
        "\n"
        "Lists the console system functions the scanned files import, i.e. everything\n"
        "that neither Windows nor the game's own DLLs provide.\n"
        "\n"
        "options:\n"
        "  --hle <dir>    compare against g6shim's replacement DLLs (the hle/ folder)\n"
        "                 and show coverage: [x] implemented  [~] stubbed  [ ] missing\n"
        "  --all          also list functions from Windows DLLs and the game's own DLLs\n"
        "  --importers    show which files import each function\n"
        "  --json         machine-readable output\n";
}

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Implemented: return "implemented";
        case Status::Stubbed: return "stubbed";
        default: return "missing";
    }
}

const char* pc_name(g6shim::PcDll pc) {
    switch (pc) {
        case g6shim::PcDll::Found: return "found";
        case g6shim::PcDll::NotFound: return "not found";
        default: return "not checked";
    }
}

const char* status_mark(Status s) {
    switch (s) {
        case Status::Implemented: return "[x]";
        case Status::Stubbed: return "[~]";
        default: return "[ ]";
    }
}

std::string percent(std::size_t part, std::size_t whole) {
    if (whole == 0) return "n/a";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.0f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
    return buf;
}

struct Totals {
    std::size_t implemented = 0, stubbed = 0, total = 0;
    void add(Status s) {
        ++total;
        if (s == Status::Implemented) ++implemented;
        if (s == Status::Stubbed) ++stubbed;
    }
};

void print_text(const g6shim::Inventory& inv, const std::optional<g6shim::hle::HleTree>& hle, bool all,
                bool importers) {
    std::cout << "g6shim scan: " << inv.files.size() << " file(s) read";
    if (!inv.failures.empty()) std::cout << ", " << inv.failures.size() << " skipped";
    if (!inv.g6shim_files.empty()) std::cout << ", " << inv.g6shim_files.size() << " g6shim DLL(s) ignored";
    std::cout << "\n\n";

    for (const auto& f : inv.files) {
        const auto& img = f.image;
        std::cout << "  " << f.path.filename().string() << "  " << g6shim::pe::machine_name(img.machine) << " "
                  << (img.pe32_plus ? "PE32+" : "PE32") << ", " << (img.is_dll() ? "DLL" : "EXE") << ", "
                  << g6shim::pe::subsystem_name(img.subsystem) << ", entry " << g6shim::pe::hex(img.entry_point_rva);
        if (!img.exports.empty()) std::cout << ", " << img.exports.size() << " exports";
        std::cout << "\n";
        for (const auto& w : img.warnings) std::cout << "      warning: " << w << "\n";
    }
    for (const auto& fail : inv.failures)
        std::cout << "  skipped " << fail.path.string() << ": " << fail.error << "\n";

    Totals overall;
    std::vector<const g6shim::ModuleUse*> needed, windows, local;
    for (const auto& [key, mod] : inv.modules) (mod.local ? local : mod.windows ? windows : needed).push_back(&mod);

    auto print_module = [&](const g6shim::ModuleUse& mod, bool tracked) {
        Totals t;
        for (const auto& [fkey, _] : mod.functions) {
            const Status s = hle ? hle->status(mod.dll, fkey) : Status::Missing;
            t.add(s);
            if (tracked) overall.add(s);
        }
        std::cout << "\n  " << mod.dll << "  (" << mod.functions.size() << ")";
        if (mod.delay_only) std::cout << "  [delay-loaded]";
        if (hle && tracked)
            std::cout << "  " << t.implemented << " implemented, " << t.stubbed << " stubbed, "
                      << percent(t.implemented, t.total);
        std::cout << "\n";
        for (const auto& [fkey, use] : mod.functions) {
            std::cout << "    ";
            if (hle && tracked) std::cout << status_mark(hle->status(mod.dll, fkey)) << " ";
            std::cout << fkey;
            if (importers) {
                std::cout << "   <-";
                for (const auto& who : use.importers) std::cout << " " << who;
            }
            std::cout << "\n";
        }
    };

    std::cout << "\nNeeds g6shim: " << inv.system_function_count() << " functions in " << needed.size() << " DLL(s)\n";
    if (needed.empty()) std::cout << "  nothing: Windows or the game itself provides every import\n";
    for (const auto* m : needed) print_module(*m, true);

    std::vector<const g6shim::ModuleUse*> gaps;
    std::size_t gap_functions = 0;
    for (const auto* m : windows) {
        const auto n = static_cast<std::size_t>(std::count_if(
            m->functions.begin(), m->functions.end(), [](const auto& f) { return f.second.missing_on_pc; }));
        if (n) {
            gaps.push_back(m);
            gap_functions += n;
        }
    }
    if (!gaps.empty()) {
        std::cout << "\nMissing from this PC's Windows DLLs: " << gap_functions << " functions in " << gaps.size()
                  << " DLL(s)\n";
        for (const auto* m : gaps) {
            std::cout << "\n  " << m->dll;
            if (m->pc == g6shim::PcDll::NotFound) std::cout << "  (DLL not on this PC, " << m->functions.size() << " functions)";
            if (m->known_dll) std::cout << "  [KnownDLL: Windows always loads its own copy]";
            std::cout << "\n";
            if (m->pc == g6shim::PcDll::NotFound) continue;
            for (const auto& [fkey, use] : m->functions) {
                if (!use.missing_on_pc) continue;
                std::cout << "    " << fkey;
                if (importers) {
                    std::cout << "   <-";
                    for (const auto& who : use.importers) std::cout << " " << who;
                }
                std::cout << "\n";
            }
        }
    }

    auto summary = [&](const char* title, const std::vector<const g6shim::ModuleUse*>& list) {
        if (list.empty()) return;
        if (all) {
            std::cout << "\n" << title << ":\n";
            for (const auto* m : list) print_module(*m, false);
        } else {
            std::cout << "\n" << title << " (--all lists the functions):";
            for (const auto* m : list) std::cout << " " << m->dll << " (" << m->functions.size() << ")";
            std::cout << "\n";
        }
    };
    summary("Provided by Windows", windows);
    summary("Ships with the game", local);

    if (hle) {
        std::cout << "\nCoverage: " << overall.implemented << "/" << overall.total << " implemented ("
                  << percent(overall.implemented, overall.total) << "), " << overall.stubbed << " stubbed, "
                  << (overall.total - overall.implemented - overall.stubbed) << " missing\n";
    }
}

void print_json(const g6shim::Inventory& inv, const std::optional<g6shim::hle::HleTree>& hle) {
    std::cout << "{\n  \"tool\": \"g6shim-scan\",\n  \"version\": " << json_str(G6SHIM_VERSION)
              << ",\n  \"files\": [";
    for (std::size_t i = 0; i < inv.files.size(); ++i) {
        const auto& f = inv.files[i];
        const auto& img = f.image;
        std::cout << (i ? "," : "") << "\n    {\"path\": " << json_str(f.path.string())
                  << ", \"machine\": " << json_str(g6shim::pe::machine_name(img.machine))
                  << ", \"format\": " << json_str(img.pe32_plus ? "PE32+" : "PE32")
                  << ", \"type\": " << json_str(img.is_dll() ? "dll" : "exe")
                  << ", \"subsystem\": " << json_str(g6shim::pe::subsystem_name(img.subsystem))
                  << ", \"entry_rva\": " << img.entry_point_rva << ", \"exports\": " << img.exports.size()
                  << ", \"warnings\": [";
        for (std::size_t w = 0; w < img.warnings.size(); ++w) std::cout << (w ? ", " : "") << json_str(img.warnings[w]);
        std::cout << "]}";
    }
    std::cout << "\n  ],\n  \"skipped\": [";
    for (std::size_t i = 0; i < inv.failures.size(); ++i)
        std::cout << (i ? "," : "") << "\n    {\"path\": " << json_str(inv.failures[i].path.string())
                  << ", \"error\": " << json_str(inv.failures[i].error) << "}";
    std::cout << "\n  ],\n  \"modules\": [";

    Totals overall;
    bool first_mod = true;
    for (const auto& [key, mod] : inv.modules) {
        const bool tracked = !mod.local && !mod.windows;
        std::cout << (first_mod ? "" : ",") << "\n    {\"dll\": " << json_str(mod.dll)
                  << ", \"local\": " << (mod.local ? "true" : "false")
                  << ", \"windows\": " << (mod.windows ? "true" : "false")
                  << ", \"delay_loaded\": " << (mod.delay_only ? "true" : "false")
                  << ", \"pc\": " << json_str(pc_name(mod.pc))
                  << ", \"known_dll\": " << (mod.known_dll ? "true" : "false") << ", \"functions\": [";
        first_mod = false;
        bool first_fn = true;
        for (const auto& [fkey, use] : mod.functions) {
            const Status s = hle && tracked ? hle->status(mod.dll, fkey) : Status::Missing;
            if (tracked) overall.add(s);
            const char* on_pc = mod.pc == g6shim::PcDll::NotChecked ? "null" : use.missing_on_pc ? "false" : "true";
            std::cout << (first_fn ? "" : ",") << "\n      {\"name\": " << json_str(use.import.name)
                      << ", \"ordinal\": " << (use.import.ordinal ? std::to_string(*use.import.ordinal) : "null")
                      << ", \"status\": " << json_str(status_name(s)) << ", \"on_pc\": " << on_pc
                      << ", \"importers\": [";
            first_fn = false;
            bool first_imp = true;
            for (const auto& who : use.importers) {
                std::cout << (first_imp ? "" : ", ") << json_str(who);
                first_imp = false;
            }
            std::cout << "]}";
        }
        std::cout << "\n    ]}";
    }
    std::cout << "\n  ],\n  \"coverage\": {\"implemented\": " << overall.implemented
              << ", \"stubbed\": " << overall.stubbed << ", \"total\": " << overall.total << "}\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    std::optional<fs::path> hle_dir;
    bool all = false, importers = false, json = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--hle" && i + 1 < argc) {
            hle_dir = argv[++i];
        } else if (a == "--all") {
            all = true;
        } else if (a == "--importers") {
            importers = true;
        } else if (a == "--json") {
            json = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            usage();
            return 2;
        } else {
            inputs.emplace_back(a);
        }
    }
    if (inputs.empty()) {
        usage();
        return 2;
    }

    try {
        auto inv = g6shim::scan_paths(inputs);
        g6shim::check_windows_dlls(inv, g6shim::system_dir(), g6shim::known_dlls());
        std::optional<g6shim::hle::HleTree> hle;
        if (hle_dir) hle = g6shim::hle::HleTree::load(*hle_dir);
        if (json) print_json(inv, hle);
        else print_text(inv, hle, all, importers);
        return inv.files.empty() ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "g6shim-scan: " << e.what() << "\n";
        return 1;
    }
}
