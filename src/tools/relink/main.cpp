// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/pe.hpp"
#include "g6shim/relink.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage() {
    std::cerr <<
        "usage: g6shim-relink <exe or dll> -o <output> [--rename <dll>[=<new name>]]... [--keep-subsystem]\n"
        "\n"
        "Writes a patched copy that Windows will load the way g6shim needs:\n"
        "  --rename <dll>   point the file's imports of <dll> at g6shim_<dll> (or the\n"
        "                   given new name), e.g. for DLLs Windows always loads from\n"
        "                   System32 (KnownDLLs) or that g6shim should intercept\n"
        "  the game console subsystem (14) becomes Windows GUI unless --keep-subsystem\n"
        "  is given.\n"
        "\n"
        "Relink from the untouched dump into your working copy; the input is never\n"
        "modified and can't be the output.\n";
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    const std::streamoff size = in.tellg();
    if (size < 0 || size > 0xffffffffll) throw std::runtime_error("cannot read " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path input, output;
    g6shim::relink::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if ((a == "-o" || a == "--out") && i + 1 < argc) {
            output = argv[++i];
        } else if (a == "--rename" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            const std::string from = spec.substr(0, eq);
            const std::string to = eq == std::string::npos ? g6shim::relink::default_new_name(from) : spec.substr(eq + 1);
            options.renames[g6shim::pe::to_lower_ascii(from)] = to;
        } else if (a == "--keep-subsystem") {
            options.fix_subsystem = false;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            usage();
            return 2;
        } else if (input.empty()) {
            input = a;
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        usage();
        return 2;
    }

    try {
        std::error_code ec;
        if (fs::exists(output, ec) && fs::equivalent(input, output, ec))
            throw std::runtime_error("refusing to write over the input; relink into a copy");

        const auto result = g6shim::relink::relink(read_file(input), options);
        for (const auto& w : result.warnings) std::cerr << "warning: " << w << "\n";

        if (output.has_parent_path()) fs::create_directories(output.parent_path());
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));
        if (!out) throw std::runtime_error("cannot write " + output.string());

        for (const auto& c : result.changes) std::cout << "  " << c << "\n";
        std::cout << "wrote " << output.string() << "\n";
        if (!options.renames.empty())
            std::cout << "next: build the renamed DLLs, e.g.  g6shim-stubgen <game folder> -o hle --forward <original dll>\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "g6shim-relink: " << e.what() << "\n";
        return 1;
    }
}
