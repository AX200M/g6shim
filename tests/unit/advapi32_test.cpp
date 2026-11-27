// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <g6shim/console_model.hpp>

#include <windows.h>

#include <cstddef>

extern "C" LSTATUS WINAPI g6shim_RegOpenKeyW(HKEY key, LPCWSTR sub, PHKEY out);
extern "C" LSTATUS WINAPI g6shim_RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY out);
extern "C" LSTATUS WINAPI g6shim_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                                  LPDWORD size);
extern "C" LSTATUS WINAPI g6shim_RegCloseKey(HKEY key);

namespace {

constexpr wchar_t kOemKeyPath[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr wchar_t kConsoleModeValue[] = L"ConsoleMode";
constexpr wchar_t kCurrentVersionPath[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion";
constexpr DWORD kSeriesXConsoleMode = 0x64;
constexpr DWORD kSeriesSConsoleMode = 0x60;

struct ConsoleRestore {
    ~ConsoleRestore() { g6shim::set_console_model(g6shim::ConsoleModel::kSeriesX); }
};

HKEY open_oem_key() {
    HKEY key = nullptr;
    if (g6shim_RegOpenKeyW(HKEY_LOCAL_MACHINE, kOemKeyPath, &key) != ERROR_SUCCESS) return nullptr;
    return key;
}

}  // namespace

TEST(advapi32_opens_the_oem_key_with_both_functions) {
    HKEY wide = nullptr;
    CHECK(g6shim_RegOpenKeyW(HKEY_LOCAL_MACHINE, kOemKeyPath, &wide) == ERROR_SUCCESS);
    CHECK(wide != nullptr);

    HKEY mixed = nullptr;
    CHECK(g6shim_RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"software\\mIcRoSoFt\\windows nt\\CURRENTVERSION\\oem", 0, KEY_READ,
                               &mixed) == ERROR_SUCCESS);
    CHECK(mixed == wide);
}

TEST(advapi32_queries_the_console_mode_for_both_models) {
    ConsoleRestore restore;
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = static_cast<DWORD>(sizeof(value));
    g6shim::set_console_model(g6shim::ConsoleModel::kSeriesS);
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_SUCCESS);
    CHECK(type == REG_DWORD);
    CHECK(size == static_cast<DWORD>(sizeof(DWORD)));
    CHECK(value == kSeriesSConsoleMode);

    type = 0;
    value = 0;
    size = static_cast<DWORD>(sizeof(value));
    g6shim::set_console_model(g6shim::ConsoleModel::kSeriesX);
    CHECK(g6shim_RegQueryValueExW(key, L"CONSOLEMODE", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_SUCCESS);
    CHECK(type == REG_DWORD);
    CHECK(size == static_cast<DWORD>(sizeof(DWORD)));
    CHECK(value == kSeriesXConsoleMode);
}

TEST(advapi32_rejects_other_value_names) {
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = static_cast<DWORD>(sizeof(value));
    CHECK(g6shim_RegQueryValueExW(key, L"ConsoleMod", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_FILE_NOT_FOUND);
    CHECK(g6shim_RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) ==
          ERROR_FILE_NOT_FOUND);
    CHECK(size == static_cast<DWORD>(sizeof(value)));
}

TEST(advapi32_reports_the_value_without_a_buffer) {
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    DWORD type = 0;
    DWORD size = 0;
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    CHECK(type == REG_DWORD);
    CHECK(size == static_cast<DWORD>(sizeof(DWORD)));

    type = 0;
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS);
    CHECK(type == REG_DWORD);
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
}

TEST(advapi32_reports_more_data_for_a_short_buffer) {
    ConsoleRestore restore;
    g6shim::set_console_model(g6shim::ConsoleModel::kSeriesX);
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    BYTE small[3] = {0xAA, 0xAA, 0xAA};
    DWORD size = static_cast<DWORD>(sizeof(small));
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, nullptr, small, &size) == ERROR_MORE_DATA);
    CHECK(size == static_cast<DWORD>(sizeof(DWORD)));
    CHECK(small[0] == 0xAA);
    CHECK(small[1] == 0xAA);
    CHECK(small[2] == 0xAA);

    DWORD whole = 0;
    size = static_cast<DWORD>(sizeof(whole));
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, nullptr, reinterpret_cast<LPBYTE>(&whole), &size) ==
          ERROR_SUCCESS);
    CHECK(size == static_cast<DWORD>(sizeof(DWORD)));
    CHECK(whole == kSeriesXConsoleMode);
}

TEST(advapi32_rejects_data_without_a_size) {
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    DWORD value = 0;
    CHECK(g6shim_RegQueryValueExW(key, kConsoleModeValue, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), nullptr) ==
          ERROR_INVALID_PARAMETER);
}

TEST(advapi32_has_no_subkeys) {
    HKEY key = open_oem_key();
    CHECK(key != nullptr);

    HKEY child = nullptr;
    CHECK(g6shim_RegOpenKeyW(key, L"Sub", &child) == ERROR_FILE_NOT_FOUND);
    CHECK(g6shim_RegOpenKeyExW(key, kOemKeyPath, 0, KEY_READ, &child) == ERROR_FILE_NOT_FOUND);
}

TEST(advapi32_closes_the_pseudo_key) {
    HKEY key = open_oem_key();
    CHECK(key != nullptr);
    CHECK(g6shim_RegCloseKey(key) == ERROR_SUCCESS);
    CHECK(g6shim_RegCloseKey(key) == ERROR_SUCCESS);
}

TEST(advapi32_passes_real_keys_to_the_host) {
    HKEY pseudo = open_oem_key();
    CHECK(pseudo != nullptr);

    HKEY software = nullptr;
    CHECK(g6shim_RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ, &software) == ERROR_SUCCESS);
    CHECK(software != nullptr);
    CHECK(software != pseudo);
    CHECK(g6shim_RegCloseKey(software) == ERROR_SUCCESS);

    HKEY version = nullptr;
    CHECK(g6shim_RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCurrentVersionPath, 0, KEY_READ, &version) == ERROR_SUCCESS);
    CHECK(version != nullptr);

    wchar_t product[256] = {};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(sizeof(product));
    CHECK(g6shim_RegQueryValueExW(version, L"ProductName", nullptr, &type, reinterpret_cast<LPBYTE>(product), &size) ==
          ERROR_SUCCESS);
    CHECK(type == REG_SZ);
    CHECK(size >= static_cast<DWORD>(sizeof(wchar_t)));
    CHECK(product[0] != L'\0');
    CHECK(g6shim_RegCloseKey(version) == ERROR_SUCCESS);
}
