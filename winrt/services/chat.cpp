// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <cstdint>

namespace g6shim::winrt {

namespace {

constexpr char kChatSession[] = "Windows.Xbox.Chat.ChatSession";
constexpr GUID kChatSessionFactory = {0x53CB6065, 0xC211, 0x4B21, {0xA3, 0xEF, 0xCD, 0x54, 0x4D, 0x22, 0xAB, 0xA1}};
constexpr std::size_t kCreate = 0;
constexpr std::size_t kSessionList = 0;

HRESULT __stdcall CreateChatSession(void*, std::uintptr_t, std::uint32_t, void** session) {
    if (!session) return E_POINTER;
    *session = make_probe(kChatSession, kNoIid);
    return S_OK;
}

HRESULT __stdcall ChatSession_get_List(void*, void** list) {
    if (!list) return E_POINTER;
    *list = make_vector();
    return S_OK;
}

}  // namespace

void register_chat() {
    register_interface(kChatSession, kChatSessionFactory, make_vtable({{kCreate, fn(&CreateChatSession)}}));
    register_interface(kChatSession, kNoIid, make_vtable({{kSessionList, fn(&ChatSession_get_List)}}));
}

}  // namespace g6shim::winrt
