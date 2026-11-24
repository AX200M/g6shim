// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include <g6shim/log.hpp>

#include <cstring>

namespace {

constexpr wchar_t kPcAudioDll[] = L"XAudio2_9.dll";
constexpr char kPcAudioCreate[] = "XAudio2Create";
constexpr char kConsoleAudioCreate[] = "CreateXAudio2Object";
constexpr HRESULT kAudioInvalidCall = static_cast<HRESULT>(0x88960001);
constexpr UINT32 kDefaultProcessor = 1;

using XAudio2CreateFn = HRESULT(WINAPI*)(void** xaudio2, UINT32 flags, UINT32 processor);

HRESULT WINAPI CreateXAudio2Object(void** xaudio2, UINT32 flags, UINT32 processor, void* extra) {
    static const XAudio2CreateFn create = [] {
        HMODULE dll = LoadLibraryW(kPcAudioDll);
        return dll ? reinterpret_cast<XAudio2CreateFn>(GetProcAddress(dll, kPcAudioCreate)) : nullptr;
    }();
    if (!create) return kAudioInvalidCall;
    HRESULT hr = create(xaudio2, 0, processor);
    if (FAILED(hr)) hr = create(xaudio2, 0, kDefaultProcessor);
    G6SHIM_INFO("CreateXAudio2Object(flags 0x%X, processor 0x%X, %p) -> PC XAudio2Create -> 0x%08lX", flags, processor,
                extra, static_cast<unsigned long>(hr));
    return hr;
}

}  // namespace

extern "C" FARPROC WINAPI g6shim_GetProcAddress(HMODULE module, LPCSTR name) {
    if (!IS_INTRESOURCE(name) && std::strcmp(name, kConsoleAudioCreate) == 0)
        return reinterpret_cast<FARPROC>(&CreateXAudio2Object);
    const FARPROC fn = GetProcAddress(module, name);
    if (!fn) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(module, path, MAX_PATH);
        if (IS_INTRESOURCE(name))
            G6SHIM_WARN("GetProcAddress(%s, #%u): not on this PC", path, static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(name)));
        else
            G6SHIM_WARN("GetProcAddress(%s, %s): not on this PC", path, name);
    }
    return fn;
}
