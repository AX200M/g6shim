// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "g6shim/hle.hpp"
#include "g6shim/inventory.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kCppSpdx[] =
    "// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors\n"
    "// SPDX-License-Identifier: LGPL-3.0-or-later\n";
constexpr char kCmakeSpdx[] =
    "# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors\n"
    "# SPDX-License-Identifier: LGPL-3.0-or-later\n";
constexpr char kStubInclude[] = "#include <g6shim/stub.hpp>\n\n";

void usage() {
    std::cerr <<
        "usage: g6shim-stubgen <game folder or .exe/.dll files...> -o <hle dir> [options]\n"
        "\n"
        "Writes a replacement DLL project for every console system DLL the game imports\n"
        "(skipping standard Windows DLLs and DLLs that ship with the game).\n"
        "Each imported function becomes a stub that logs its first call and returns 0.\n"
        "Folders that already exist are extended, never overwritten.\n"
        "\n"
        "options:\n"
        "  -o, --out <dir>  where module folders go (normally the repo's hle/ folder)\n"
        "  --dll <name>     only this DLL (repeatable); overrides the skipping above\n"
        "  --forward <dll>  forward functions this PC DLL exports under the same name\n"
        "                   (e.g. kernel32.dll) instead of stubbing them; repeatable,\n"
        "                   first match wins. Looked up in System32 unless a path is given.\n";
}

std::string safe_dir_name(const std::string& dll) {
    std::string stem = g6shim::pe::to_lower_ascii(fs::path(dll).stem().string());
    for (char& c : stem)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) c = '_';
    if (stem.empty() || stem == "." || stem == "..") stem = "module";
    return stem;
}

std::string target_name(const std::string& dir) {
    std::string t = "hle_" + dir;
    for (char& c : t)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
    return t;
}

std::string cpp_literal(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool write_new(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
    return static_cast<bool>(out);
}

bool append(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << content;
    return static_cast<bool>(out);
}

bool ends_with_newline(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() <= 0) return true;
    in.seekg(-1, std::ios::end);
    return in.get() == '\n';
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct ForwardTarget {
    std::string module;
    std::set<std::string> names;
};

ForwardTarget load_forward_target(const std::string& arg) {
    fs::path path(arg);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) path = g6shim::system_dir() / arg;
    const auto image = g6shim::pe::parse_file(path);
    ForwardTarget t;
    t.module = path.stem().string();
    for (const auto& ex : image.exports)
        if (!ex.name.empty()) t.names.insert(ex.name);
    if (t.names.empty()) throw std::runtime_error(path.string() + " exports no named functions");
    return t;
}

struct Generated {
    std::string def_lines;
    std::string stub_lines;
    std::size_t count = 0;
    std::size_t forwarded = 0;
    std::vector<std::string> skipped;
};

Generated generate(const g6shim::ModuleUse& mod, const std::set<std::string>& existing_keys, std::size_t first_id,
                   const std::vector<ForwardTarget>& forwards) {
    Generated g;
    std::size_t id = first_id;
    for (const auto& [key, use] : mod.functions) {
        if (existing_keys.count(key)) continue;
        const ForwardTarget* target = nullptr;
        if (!use.import.ordinal && g6shim::hle::is_def_safe_name(use.import.name)) {
            for (const auto& t : forwards) {
                if (t.names.count(use.import.name)) {
                    target = &t;
                    break;
                }
            }
        }
        if (target) {
            g.def_lines += "    " + use.import.name + "=" + target->module + "." + use.import.name + "\n";
            ++g.forwarded;
            continue;
        }
        const std::string symbol = std::string(g6shim::hle::kStubPrefix) + std::to_string(id);
        if (use.import.ordinal) {
            g.def_lines += "    " + symbol + " @" + std::to_string(*use.import.ordinal) + " NONAME\n";
        } else if (g6shim::hle::is_def_safe_name(use.import.name)) {
            g.def_lines += "    " + use.import.name + "=" + symbol + "\n";
        } else {
            g.skipped.push_back(use.import.name);
            continue;
        }
        g.stub_lines += std::string(g6shim::hle::kStubMacro) + std::to_string(id) + ", " + cpp_literal(mod.dll) + ", " + cpp_literal(key) + ")\n";
        ++id;
        ++g.count;
    }
    return g;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    fs::path out_dir;
    std::set<std::string> only;
    std::vector<std::string> forward_args;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if ((a == "-o" || a == "--out") && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--dll" && i + 1 < argc) {
            only.insert(g6shim::pe::to_lower_ascii(argv[++i]));
        } else if (a == "--forward" && i + 1 < argc) {
            forward_args.emplace_back(argv[++i]);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            usage();
            return 2;
        } else {
            inputs.emplace_back(a);
        }
    }
    if (inputs.empty() || out_dir.empty()) {
        usage();
        return 2;
    }

    try {
        std::vector<ForwardTarget> forwards;
        for (const auto& f : forward_args) forwards.push_back(load_forward_target(f));
        const auto inv = g6shim::scan_paths(inputs);
        for (const auto& fail : inv.failures)
            std::cerr << "skipped " << fail.path.string() << ": " << fail.error << "\n";
        if (inv.files.empty()) {
            std::cerr << "g6shim-stubgen: no readable .exe/.dll files found\n";
            return 1;
        }
        fs::create_directories(out_dir);

        std::set<std::string> used_dirs;
        int touched = 0;
        for (const auto& [key, mod] : inv.modules) {
            if (only.empty() ? (mod.local || mod.windows) : !only.count(key)) continue;

            const std::string base = safe_dir_name(mod.dll);
            std::string dir_name = base;
            for (int n = 2;; ++n) {
                const fs::path candidate_def = out_dir / dir_name / (dir_name + ".def");
                const bool taken_by_other =
                    fs::exists(candidate_def) &&
                    g6shim::pe::to_lower_ascii(g6shim::hle::DefFile::load(candidate_def).dll) != key;
                if (!used_dirs.count(dir_name) && !taken_by_other) break;
                dir_name = base + "_" + std::to_string(n);
            }
            used_dirs.insert(dir_name);

            const fs::path dir = out_dir / dir_name;
            const fs::path def_path = dir / (dir_name + ".def");
            const fs::path stubs_path = dir / (dir_name + "_stubs.cpp");

            std::set<std::string> existing;
            std::size_t first_id = 0;
            const bool exists = fs::exists(def_path);
            if (exists) {
                const auto def = g6shim::hle::DefFile::load(def_path);
                for (const auto& ex : def.exports) existing.insert(ex.key());
                first_id = g6shim::hle::next_stub_id(def, read_text(stubs_path));
            }

            const Generated g = generate(mod, existing, first_id, forwards);
            for (const auto& s : g.skipped)
                std::cerr << "  warning: " << mod.dll << ": can't export \"" << s << "\" from a .def file; skipped\n";

            if (!exists) {
                fs::create_directories(dir);
                std::ostringstream def;
                def << "LIBRARY \"" << mod.dll << "\"\n"
                    << "EXPORTS\n"
                    << g.def_lines;
                std::ostringstream stubs;
                stubs << kCppSpdx << kStubInclude << g.stub_lines;
                std::ostringstream cmake;
                cmake << kCmakeSpdx
                      << "file(GLOB sources CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n"
                      << "g6shim_add_hle_module(" << target_name(dir_name) << "\n"
                      << "    DLL \"" << mod.dll << "\"\n"
                      << "    DEF \"${CMAKE_CURRENT_SOURCE_DIR}/" << dir_name << ".def\"\n"
                      << "    SOURCES ${sources})\n";
                if (!write_new(def_path, def.str()) || !write_new(stubs_path, stubs.str()) ||
                    !write_new(dir / "CMakeLists.txt", cmake.str()))
                    throw std::runtime_error("cannot write to " + dir.string());
                std::cout << mod.dll << ": created " << dir.string() << " with " << g.count << " stubs, "
                          << g.forwarded << " forwarders\n";
                ++touched;
            } else if (g.count == 0 && g.forwarded == 0) {
                std::cout << mod.dll << ": up to date\n";
            } else {
                if (!fs::exists(stubs_path)) {
                    if (!write_new(stubs_path, std::string(kCppSpdx) + kStubInclude))
                        throw std::runtime_error("cannot write " + stubs_path.string());
                }
                const std::string def_sep = ends_with_newline(def_path) ? "" : "\n";
                const std::string stub_sep = ends_with_newline(stubs_path) ? "" : "\n";
                if (!append(def_path, def_sep + g.def_lines) || !append(stubs_path, stub_sep + g.stub_lines))
                    throw std::runtime_error("cannot update " + dir.string());
                std::cout << mod.dll << ": added " << g.count << " stubs, " << g.forwarded << " forwarders\n";
                ++touched;
            }
        }
        if (touched == 0 && inv.system_function_count() == 0)
            std::cout << "no system imports found\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "g6shim-stubgen: " << e.what() << "\n";
        return 1;
    }
}
