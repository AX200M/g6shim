// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/inventory.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace g6shim {

namespace {

bool looks_like_pe_file(const fs::path& p) {
    const auto ext = pe::to_lower_ascii(p.extension().string());
    return ext == ".exe" || ext == ".dll";
}

std::vector<fs::path> collect(const std::vector<fs::path>& inputs, std::vector<ScanFailure>& failures) {
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
                const fs::directory_entry& entry = *it;
                std::error_code fec;
                if (entry.is_directory(fec) && pe::to_lower_ascii(entry.path().filename().string()) == kConsoleImageFolder) {
                    failures.push_back({entry.path(), "console OS image, not the game (pass this folder itself to scan it)"});
                    it.disable_recursion_pending();
                } else if (entry.is_regular_file(fec) && looks_like_pe_file(entry.path())) {
                    out.push_back(entry.path());
                }
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

constexpr std::array<std::string_view, 72> kWindowsDlls = {
    "advapi32.dll", "bcrypt.dll", "cfgmgr32.dll", "combase.dll", "comctl32.dll", "comdlg32.dll",
    "crypt32.dll", "d2d1.dll", "d3d10.dll", "d3d11.dll", "d3d12.dll", "d3d9.dll", "d3dcompiler_47.dll",
    "dbghelp.dll", "dinput8.dll", "dsound.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll", "gdi32.dll",
    "hid.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll", "mfplat.dll",
    "mfreadwrite.dll", "msvcp_win.dll", "msvcrt.dll", "ncrypt.dll", "normaliz.dll", "ntdll.dll",
    "ole32.dll", "oleaut32.dll", "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll",
    "rpcrt4.dll", "secur32.dll", "sechost.dll", "setupapi.dll", "shcore.dll", "shell32.dll",
    "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "uxtheme.dll", "version.dll",
    "winhttp.dll", "wininet.dll", "winmm.dll", "wintrust.dll", "ws2_32.dll", "wsock32.dll",
    "xaudio2_8.dll", "xaudio2_9.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
    "mswsock.dll", "winspool.drv", "netapi32.dll", "dnsapi.dll", "wtsapi32.dll", "msimg32.dll",
    "glu32.dll", "avrt.dll", "mmdevapi.dll", "audioses.dll"};

constexpr std::array<std::string_view, 7> kWindowsPrefixes = {
    "api-ms-win-", "ext-ms-win-", "msvcr", "msvcp", "vcruntime", "concrt", "vcomp"};

constexpr std::array<std::string_view, 2> kApiSetPrefixes = {"api-ms-win-", "ext-ms-win-"};

constexpr char kKnownDllsKey[] = R"(SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs)";
constexpr char kAlwaysMapped[] = "ntdll.dll";

bool is_api_set(const std::string& name) {
    return std::any_of(kApiSetPrefixes.begin(), kApiSetPrefixes.end(),
                       [&](std::string_view prefix) { return name.rfind(prefix, 0) == 0; });
}

}  // namespace

bool is_windows_dll(const std::string& name) {
    const std::string lower = pe::to_lower_ascii(name);
    if (std::find(kWindowsDlls.begin(), kWindowsDlls.end(), lower) != kWindowsDlls.end()) return true;
    for (auto prefix : kWindowsPrefixes)
        if (lower.rfind(prefix, 0) == 0) return true;
    if (const fs::path dir = system_dir(); !dir.empty()) {
        std::error_code ec;
        if (fs::exists(dir / lower, ec)) return true;
    }
    return false;
}

fs::path system_dir() {
    if (const char* root = std::getenv("SystemRoot"); root && *root) return fs::path(root) / "System32";
    return {};
}

std::set<std::string> known_dlls() {
    std::set<std::string> out;
    out.insert(kAlwaysMapped);
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKnownDllsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        char name[256];
        DWORD name_len = sizeof name;
        BYTE data[512];
        DWORD data_len = sizeof data - 1;
        DWORD type = 0;
        const LONG r = RegEnumValueA(key, i, name, &name_len, nullptr, &type, data, &data_len);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        data[data_len] = 0;
        const std::string dll = pe::to_lower_ascii(reinterpret_cast<const char*>(data));
        if (!dll.empty() && dll.find_first_of("\\/%") == std::string::npos) out.insert(dll);
    }
    RegCloseKey(key);
    return out;
}

void check_windows_dlls(Inventory& inv, const fs::path& dir, const std::set<std::string>& known) {
    for (auto& [key, mod] : inv.modules) {
        if (!mod.windows) continue;
        mod.known_dll = known.count(key) != 0;
        if (dir.empty() || is_api_set(key)) continue;

        std::error_code ec;
        const fs::path file = dir / key;
        if (!fs::is_regular_file(file, ec)) {
            mod.pc = PcDll::NotFound;
            for (auto& [_, fn] : mod.functions) fn.missing_on_pc = true;
            continue;
        }
        pe::Image image;
        try {
            image = pe::parse_file(file);
        } catch (const pe::ParseError&) {
            continue;
        }
        std::set<std::string> names;
        std::set<std::uint32_t> ordinals;
        for (const auto& ex : image.exports) {
            if (!ex.name.empty()) names.insert(ex.name);
            ordinals.insert(ex.ordinal);
        }
        mod.pc = PcDll::Found;
        for (auto& [_, fn] : mod.functions)
            fn.missing_on_pc = fn.import.ordinal ? ordinals.count(*fn.import.ordinal) == 0
                                                 : names.count(fn.import.name) == 0;
    }
}

std::size_t Inventory::system_function_count() const {
    std::size_t n = 0;
    for (const auto& [_, m] : modules)
        if (!m.local && !m.windows) n += m.functions.size();
    return n;
}

Inventory scan_paths(const std::vector<fs::path>& inputs) {
    Inventory inv;
    for (const auto& path : collect(inputs, inv.failures)) {
        try {
            pe::Image image = pe::parse_file(path);
            const bool ours =
                pe::to_lower_ascii(path.filename().string()) == kRuntimeDll ||
                std::any_of(image.imports.begin(), image.imports.end(),
                            [](const pe::ImportModule& m) { return pe::to_lower_ascii(m.dll) == kRuntimeDll; });
            if (ours) inv.g6shim_files.push_back(path);
            else inv.files.push_back({path, std::move(image)});
        } catch (const pe::ParseError& e) {
            inv.failures.push_back({path, e.what()});
        }
    }

    std::set<std::string> local_names;
    for (const auto& f : inv.files) local_names.insert(pe::to_lower_ascii(f.path.filename().string()));

    for (const auto& f : inv.files) {
        const std::string importer = f.path.filename().string();
        for (const auto& mod : f.image.imports) {
            const std::string key = pe::to_lower_ascii(mod.dll);
            auto [it, inserted] = inv.modules.try_emplace(key);
            ModuleUse& use = it->second;
            if (inserted) {
                use.dll = mod.dll;
                use.local = local_names.count(key) != 0;
                use.windows = !use.local && is_windows_dll(key);
            }
            if (!mod.delay_loaded) use.delay_only = false;
            for (const auto& fn : mod.functions) {
                auto [fit, finserted] = use.functions.try_emplace(fn.display());
                if (finserted) fit->second.import = fn;
                fit->second.importers.insert(importer);
                if (!mod.delay_loaded) fit->second.delay_only = false;
            }
        }
    }
    return inv;
}

}  // namespace g6shim
