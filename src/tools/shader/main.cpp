// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <crtdbg.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "g6shim/shader/scan.hpp"

namespace fs = std::filesystem;

namespace {

void usage() {
    std::cerr <<
        "usage: g6shim-shader <files or folders...> [options]\n"
        "\n"
        "Finds the shader containers a game ships, decodes their native programs,\n"
        "translates them and counts the unique shaders per stage.\n"
        "\n"
        "options:\n"
        "  --json <out.json>     write the unique per-stage counts as JSON\n"
        "  --title-id <8 hex>    title ID to put in the JSON\n"
        "  --out <dir>           write each translated PC container as <hash>.dxil\n"
        "  -v                    list files and the first failure per reason\n";
}

std::optional<fs::path> repository_root(fs::path start) {
    std::error_code ec;
    start = fs::weakly_canonical(start, ec);
    if (ec) return std::nullopt;
    for (;;) {
        if (fs::exists(start / L".git", ec)) return start;
        const fs::path parent = start.parent_path();
        if (parent.empty() || parent == start) return std::nullopt;
        start = parent;
    }
}

bool same_component(const fs::path& left, const fs::path& right) {
    const std::wstring a = left.native();
    const std::wstring b = right.native();
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index)
        if (std::towlower(a[index]) != std::towlower(b[index])) return false;
    return true;
}

bool within(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) resolved = fs::absolute(path, ec).lexically_normal();
    auto component = resolved.begin();
    for (auto root_component = root.begin(); root_component != root.end(); ++root_component, ++component) {
        if (component == resolved.end() || !same_component(*component, *root_component)) return false;
    }
    return true;
}

bool inside_repository(const fs::path& path, const std::optional<fs::path>& tool_dir) {
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    if (!ec) {
        if (const auto root = repository_root(current))
            if (within(path, *root)) return true;
    }
    if (tool_dir) {
        if (const auto root = repository_root(*tool_dir))
            if (within(path, *root)) return true;
    }
    return false;
}

class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    ~MappedFile() { close(); }

    bool open(const fs::path& path) {
        close();
        file_ = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error_ = GetLastError();
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) {
            error_ = GetLastError();
            close();
            return false;
        }
        if (size.QuadPart <= 0) return true;
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            error_ = GetLastError();
            close();
            return false;
        }
        data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (data_ == nullptr) {
            error_ = GetLastError();
            close();
            return false;
        }
        size_ = static_cast<std::size_t>(size.QuadPart);
        return true;
    }

    std::span<const std::uint8_t> bytes() const {
        if (data_ == nullptr || size_ == 0) return {};
        return std::span<const std::uint8_t>(data_, size_);
    }

    DWORD error() const { return error_; }

private:
    void close() {
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        size_ = 0;
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    DWORD error_ = 0;
};

struct InputFailure {
    fs::path path;
    std::string error;
};

std::vector<fs::path> collect(const std::vector<fs::path>& inputs, std::vector<InputFailure>& failures) {
    std::vector<fs::path> out;
    for (const auto& input : inputs) {
        std::error_code ec;
        if (fs::is_directory(input, ec)) {
            fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                failures.push_back({input, ec.message()});
                continue;
            }
            for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    failures.push_back({input, ec.message()});
                    break;
                }
                std::error_code file_ec;
                if (it->is_regular_file(file_ec)) out.push_back(it->path());
            }
        } else if (fs::is_regular_file(input, ec)) {
            out.push_back(input);
        } else {
            failures.push_back({input, "not found"});
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<std::string> normalise_title_id(const std::string& text) {
    if (text.size() != 8) return std::nullopt;
    std::string out = text;
    for (char& c : out) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return std::nullopt;
        }
    }
    return out;
}

bool write_json(const fs::path& path, const std::optional<std::string>& title_id,
                const g6shim::shader::ShaderTotals& totals) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\n  \"title_id\": ";
    if (title_id) out << '"' << *title_id << '"';
    else out << "null";
    out << ",\n  \"stages\": {";
    bool first = true;
    for (const auto& [stage, counts] : totals.stages) {
        out << (first ? "\n" : ",\n") << "    \"" << stage << "\": {\"total\": " << counts.total
            << ", \"decoded\": " << counts.decoded << ", \"translated\": " << counts.translated
            << ", \"failed\": " << counts.failed << "}";
        first = false;
    }
    if (!first) out << "\n  ";
    out << "}\n}\n";
    out.flush();
    return static_cast<bool>(out);
}

void print_totals(const g6shim::shader::ShaderTotals& totals, bool verbose) {
    std::cout << "g6shim-shader: " << totals.containers << " containers, " << totals.unique << " unique, in "
              << totals.files << " file(s)\n";
    std::uint64_t translated = 0;
    std::uint64_t decoded = 0;
    std::uint64_t failed = 0;
    for (const auto& [stage, counts] : totals.stages) {
        std::cout << "  " << stage << ": " << counts.translated << " translated, " << counts.decoded << " decoded, "
                  << counts.failed << " failed of " << counts.total << "\n";
        translated += counts.translated;
        decoded += counts.decoded;
        failed += counts.failed;
    }
    if (totals.unique != 0)
        std::cout << "  " << totals.unique << " unique: " << decoded << " decoded, " << failed << " failed, "
                  << translated << " translated\n";
    else
        std::cout << "  none found: shaders may sit in compressed or encrypted archives\n";
    if (verbose) {
        for (const auto& [reason, failure] : totals.failures)
            std::cout << "  failed " << reason << " x" << failure.count << ": " << failure.first << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::vector<fs::path> inputs;
    std::optional<fs::path> json_path;
    std::optional<fs::path> out_dir;
    std::optional<std::string> title_id;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            usage();
            return 0;
        } else if (argument == "--json") {
            if (i + 1 >= argc) {
                std::cerr << "g6shim-shader: --json needs an output file\n";
                return 2;
            }
            json_path = fs::path(argv[++i]);
        } else if (argument == "--title-id") {
            if (i + 1 >= argc) {
                std::cerr << "g6shim-shader: --title-id needs 8 hex digits\n";
                return 2;
            }
            title_id = normalise_title_id(argv[++i]);
            if (!title_id) {
                std::cerr << "g6shim-shader: --title-id needs 8 hex digits\n";
                return 2;
            }
        } else if (argument == "--out") {
            if (i + 1 >= argc) {
                std::cerr << "g6shim-shader: --out needs an output folder\n";
                return 2;
            }
            out_dir = fs::path(argv[++i]);
        } else if (argument == "-v") {
            verbose = true;
        } else if (!argument.empty() && argument[0] == '-') {
            std::cerr << "unknown option: " << argument << "\n";
            usage();
            return 2;
        } else {
            inputs.emplace_back(argument);
        }
    }
    if (inputs.empty()) {
        usage();
        return 2;
    }

    std::optional<fs::path> tool_dir;
    {
        std::error_code ec;
        const fs::path executable = fs::absolute(fs::path(argv[0]), ec);
        if (!ec) tool_dir = executable.parent_path();
    }
    if (out_dir) {
        if (inside_repository(*out_dir, tool_dir)) {
            std::cerr << "g6shim-shader: refusing to write outputs inside the repository\n";
            return 2;
        }
        std::error_code ec;
        fs::create_directories(*out_dir, ec);
        if (ec || !fs::is_directory(*out_dir, ec)) {
            std::cerr << "g6shim-shader: cannot create " << out_dir->string() << "\n";
            return 2;
        }
    }

    try {
        std::vector<InputFailure> failures;
        const std::vector<fs::path> files = collect(inputs, failures);
        for (const InputFailure& failure : failures)
            std::cerr << "g6shim-shader: skipped " << failure.path.string() << ": " << failure.error << "\n";

        g6shim::shader::ShaderScanner scanner;
        std::mutex output_mutex;
        std::string first_output_error;
        std::uint64_t output_failures = 0;
        if (out_dir) {
            const fs::path directory = *out_dir;
            scanner.set_translated_writer([directory, &output_mutex, &first_output_error, &output_failures](
                                               std::string_view hash, std::span<const std::uint8_t> dxil) {
                const fs::path path = directory / (std::string(hash) + ".dxil");
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(reinterpret_cast<const char*>(dxil.data()),
                              static_cast<std::streamsize>(dxil.size()));
                    out.flush();
                }
                if (out) return;
                std::lock_guard<std::mutex> lock(output_mutex);
                if (output_failures == 0) first_output_error = path.string();
                ++output_failures;
            });
        }
        std::size_t read = 0;
        for (const fs::path& path : files) {
            MappedFile file;
            if (!file.open(path)) {
                std::cerr << "g6shim-shader: cannot read " << path.string() << " (error " << file.error() << ")\n";
                continue;
            }
            ++read;
            const g6shim::shader::FileCounts counts = scanner.add_file(file.bytes());
            if (verbose && counts.containers != 0)
                std::cout << path.string() << ": " << counts.containers << " container(s), " << counts.unique
                          << " new\n";
        }
        if (read == 0) {
            std::cerr << "g6shim-shader: no readable files\n";
            return 1;
        }

        const g6shim::shader::ShaderTotals& totals = scanner.totals();
        print_totals(totals, verbose);

        if (json_path && !write_json(*json_path, title_id, totals)) {
            std::cerr << "g6shim-shader: cannot write " << json_path->string() << "\n";
            return 1;
        }
        if (output_failures != 0) {
            std::cerr << "g6shim-shader: " << output_failures << " translated container(s) could not be written; first "
                      << first_output_error << "\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g6shim-shader: " << error.what() << "\n";
        return 1;
    }
}
