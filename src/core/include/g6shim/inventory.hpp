// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "g6shim/pe.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace g6shim {

struct ScannedFile {
    std::filesystem::path path;
    pe::Image image;
};

struct ScanFailure {
    std::filesystem::path path;
    std::string error;
};

struct FunctionUse {
    pe::Import import;
    std::set<std::string> importers;
    bool delay_only = true;
    bool missing_on_pc = false;
};

enum class PcDll { NotChecked, Found, NotFound };

struct ModuleUse {
    std::string dll;
    bool local = false;
    bool windows = false;
    bool delay_only = true;
    PcDll pc = PcDll::NotChecked;
    bool known_dll = false;
    std::map<std::string, FunctionUse> functions;
};

struct Inventory {
    std::vector<ScannedFile> files;
    std::vector<ScanFailure> failures;
    std::vector<std::filesystem::path> g6shim_files;
    std::map<std::string, ModuleUse> modules;

    std::size_t system_function_count() const;
};

inline constexpr char kConsoleImageFolder[] = "embeddedxvd";
inline constexpr char kRuntimeDll[] = "g6shim_runtime.dll";

bool is_windows_dll(const std::string& name);

Inventory scan_paths(const std::vector<std::filesystem::path>& inputs);

void check_windows_dlls(Inventory& inv, const std::filesystem::path& system_dir,
                        const std::set<std::string>& known_dlls);

std::filesystem::path system_dir();

std::set<std::string> known_dlls();

}  // namespace g6shim
