// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <string>

namespace {

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    return L"\"" + arg + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::wstring exe = argv[1];
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    std::wstring game_cmd = quote(exe);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION game{};
    if (!CreateProcessW(exe.c_str(), game_cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(),
                        &si, &game))
        return 3;

    std::wstring cmd;
    for (int i = 2; i < argc; ++i) cmd += quote(argv[i]) + L" ";
    cmd += L"-p " + std::to_wstring(game.dwProcessId) + L" -tid " + std::to_wstring(game.dwThreadId);
    PROCESS_INFORMATION debugger{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &debugger)) {
        TerminateProcess(game.hProcess, 1);
        return 4;
    }
    WaitForSingleObject(debugger.hProcess, 120000);
    DWORD code = 5;
    GetExitCodeProcess(debugger.hProcess, &code);
    if (WaitForSingleObject(game.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(game.hProcess, 1);
    return static_cast<int>(code);
}
