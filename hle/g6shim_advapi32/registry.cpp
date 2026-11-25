// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/console_model.hpp>
#include <g6shim/log.hpp>

#include <windows.h>

#include <atomic>
#include <cstring>
#include <cwchar>

namespace {

struct ConsoleKey {
    unsigned char reserved;
};

ConsoleKey g_console_key{};

constexpr wchar_t kOemKeyPath[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
constexpr wchar_t kConsoleModeValue[] = L"ConsoleMode";
constexpr DWORD kSeriesXConsoleMode = 0x64;
constexpr DWORD kSeriesSConsoleMode = 0x60;
constexpr DWORD kConsoleModeSize = 4;

static_assert(sizeof(DWORD) == kConsoleModeSize);

HKEY console_key_handle() {
    return reinterpret_cast<HKEY>(&g_console_key);
}

bool is_console_key(HKEY key) {
    return key == console_key_handle();
}

bool is_oem_key(LPCWSTR sub) {
    return sub != nullptr && _wcsicmp(sub, kOemKeyPath) == 0;
}

DWORD console_mode_value(g6shim::ConsoleModel model) {
    return model == g6shim::ConsoleModel::kSeriesS ? kSeriesSConsoleMode : kSeriesXConsoleMode;
}

void log_console_mode_once(g6shim::ConsoleModel model) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_relaxed)) return;
    G6SHIM_INFO("device type: ConsoleMode 0x%02X (%s)", static_cast<unsigned>(console_mode_value(model)),
                g6shim::console_model_name(model));
}

}  // namespace

extern "C" LSTATUS WINAPI g6shim_RegOpenKeyW(HKEY key, LPCWSTR sub, PHKEY out) {
    if (is_console_key(key)) return ERROR_FILE_NOT_FOUND;
    if (key == HKEY_LOCAL_MACHINE && out != nullptr && is_oem_key(sub)) {
        *out = console_key_handle();
        return ERROR_SUCCESS;
    }
    return ::RegOpenKeyW(key, sub, out);
}

extern "C" LSTATUS WINAPI g6shim_RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY out) {
    if (is_console_key(key)) return ERROR_FILE_NOT_FOUND;
    if (key == HKEY_LOCAL_MACHINE && out != nullptr && is_oem_key(sub)) {
        *out = console_key_handle();
        return ERROR_SUCCESS;
    }
    return ::RegOpenKeyExW(key, sub, options, access, out);
}

extern "C" LSTATUS WINAPI g6shim_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                                                  LPDWORD size) {
    if (!is_console_key(key)) return ::RegQueryValueExW(key, name, reserved, type, data, size);
    if (name == nullptr || _wcsicmp(name, kConsoleModeValue) != 0) return ERROR_FILE_NOT_FOUND;
    if (data != nullptr && size == nullptr) return ERROR_INVALID_PARAMETER;

    const g6shim::ConsoleModel model = g6shim::console_model();
    log_console_mode_once(model);

    if (type != nullptr) *type = REG_DWORD;
    if (size == nullptr) return ERROR_SUCCESS;
    const DWORD room = *size;
    *size = kConsoleModeSize;
    if (data == nullptr) return ERROR_SUCCESS;
    if (room < kConsoleModeSize) return ERROR_MORE_DATA;

    const DWORD value = console_mode_value(model);
    std::memcpy(data, &value, kConsoleModeSize);
    return ERROR_SUCCESS;
}

extern "C" LSTATUS WINAPI g6shim_RegCloseKey(HKEY key) {
    if (is_console_key(key)) return ERROR_SUCCESS;
    return ::RegCloseKey(key);
}
