// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/log.hpp>

#include <malloc.h>
#include <mutex>
#include <cstring>
#include <set>
#include <utility>

namespace {

constexpr DWORD kPcTypes = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_RESET_UNDO | MEM_TOP_DOWN | MEM_WRITE_WATCH |
                           MEM_PHYSICAL | MEM_LARGE_PAGES;
constexpr DWORD kBaseProtections = 0xFF;
constexpr DWORD kPcProtections = kBaseProtections | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;
constexpr SIZE_T kPageAlignment = 4096;
constexpr SIZE_T kSmallAlignment = 64;

void log_once(DWORD type, DWORD protect) {
    static std::mutex mutex;
    static std::set<std::pair<DWORD, DWORD>> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert({type, protect}).second)
        G6SHIM_INFO("VirtualAlloc: console bits type 0x%lX, protection 0x%lX dropped", type & ~kPcTypes,
                    protect & ~kPcProtections);
}

}  // namespace

extern "C" LPVOID WINAPI g6shim_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (void* p = VirtualAlloc(address, size, type, protect)) return p;
    const bool console_bits = (type & ~kPcTypes) || (protect & ~kPcProtections);
    if (!console_bits && !(type & MEM_LARGE_PAGES)) return nullptr;
    if (console_bits) log_once(type, protect);
    DWORD pc_type = type & kPcTypes;
    DWORD pc_protect = protect & kPcProtections;
    if (!(pc_protect & kBaseProtections)) pc_protect |= PAGE_READWRITE;
    if (void* p = VirtualAlloc(address, size, pc_type, pc_protect)) return p;
    if (!(pc_type & MEM_LARGE_PAGES)) return nullptr;
    return VirtualAlloc(address, size, pc_type & ~MEM_LARGE_PAGES, pc_protect);
}

extern "C" LPVOID WINAPI g6shim_XMemAlloc(SIZE_T size, ULONGLONG attributes) {
    const SIZE_T alignment = size >= kPageAlignment ? kPageAlignment : kSmallAlignment;
    void* p = _aligned_malloc(size ? size : 1, alignment);
    if (p) std::memset(p, 0, size);
    static bool logged = false;
    if (!logged) {
        logged = true;
        G6SHIM_INFO("XMemAlloc(%zu, attributes 0x%llX) -> %p (first call)", size, attributes, p);
    }
    return p;
}

extern "C" void WINAPI g6shim_XMemFree(LPVOID p, ULONGLONG) { _aligned_free(p); }
