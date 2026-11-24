// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

extern "C" {
__declspec(dllimport) int ConsoleInit();
__declspec(dllimport) int ConsoleGetValue(int key);
__declspec(dllimport) int ConsoleSecret();
__declspec(dllimport) unsigned long GetCurrentProcessId();
__declspec(dllimport) unsigned long timeGetTime();
__declspec(dllimport) int GameLibAdd(int a, int b);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
}

int main() {
    std::printf("ConsoleInit -> %d\n", ConsoleInit());
    std::printf("ConsoleGetValue(5) -> %d\n", ConsoleGetValue(5));
    std::printf("ConsoleSecret -> %d\n", ConsoleSecret());
    std::printf("GetCurrentProcessId() != 0 -> %d\n", GetCurrentProcessId() != 0);
    std::printf("timeGetTime() != 0 -> %d\n", timeGetTime() != 0);
    std::printf("GameLibAdd(2, 3) -> %d\n", GameLibAdd(2, 3));
    Sleep(1);
    std::printf("Sleep(1) returned\n");
    return 0;
}
