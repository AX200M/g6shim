// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/async.hpp>
#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>
#include <winstring.h>

#include <g6shim/log.hpp>

#include <atomic>
#include <cstdint>
#include <iterator>
#include <string>

namespace g6shim::winrt {

namespace {

constexpr char kUser[] = "Windows.Xbox.System.User";
constexpr char kDisplayInfo[] = "Windows.Xbox.System.UserDisplayInfo";
constexpr GUID kStatics = {0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};

constexpr std::uint32_t kLocalUserId = 1;
constexpr wchar_t kLocalUserNumber[] = L"1000000000000001";
constexpr wchar_t kPlayerName[] = L"Player";
constexpr std::uint32_t kStaticsInitialValue = 0;
constexpr HRESULT kNetworkUnreachable = static_cast<HRESULT>(0x800704CF);
constexpr int kLoggedTokenRequests = 4;
constexpr UINT32 kMaxLoggedChars = 160;

namespace user {
constexpr std::size_t kId = 0;
constexpr std::size_t kList2 = 2;
constexpr std::size_t kDisplayInfo3 = 3;
constexpr std::size_t kFlag4 = 4;
constexpr std::size_t kSignedIn5 = 5;
constexpr std::size_t kObject7 = 7;
constexpr std::size_t kName8 = 8;
constexpr std::size_t kNumber9 = 9;
constexpr std::size_t kTokenRequest = 10;
constexpr std::size_t kTokenRequestWithBody = 12;
}  // namespace user

namespace display_info {
constexpr std::size_t kName0 = 0;
constexpr std::size_t kValue1 = 1;
constexpr std::size_t kName2 = 2;
constexpr std::size_t kName3 = 3;
constexpr std::size_t kValue4 = 4;
constexpr std::size_t kValue5 = 5;
constexpr std::size_t kList6 = 6;
}  // namespace display_info

namespace statics {
constexpr std::size_t kValue0 = 0;
constexpr std::size_t kUsers = 3;
}  // namespace statics

HRESULT string_result(const wchar_t* text, std::size_t length, HSTRING* out) {
    if (!out) return E_POINTER;
    return WindowsCreateString(text, static_cast<UINT32>(length), out);
}

HRESULT __stdcall get_Id(void*, std::uint32_t* id) {
    if (!id) return E_POINTER;
    *id = kLocalUserId;
    return S_OK;
}

HRESULT __stdcall get_Number(void*, HSTRING* value) {
    return string_result(kLocalUserNumber, std::size(kLocalUserNumber) - 1, value);
}

HRESULT __stdcall get_DisplayInfo(void*, void** value) {
    if (!value) return E_POINTER;
    *value = make_probe(kDisplayInfo, kNoIid);
    return S_OK;
}

HRESULT __stdcall get_PlayerName(void*, HSTRING* value) {
    return string_result(kPlayerName, std::size(kPlayerName) - 1, value);
}

HRESULT __stdcall get_Zero32(void*, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

HRESULT __stdcall get_EmptyList(void*, void** value) {
    if (!value) return E_POINTER;
    *value = make_empty_vector_view();
    return S_OK;
}

HRESULT __stdcall get_NullObject(void*, void** value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    return S_OK;
}

HRESULT __stdcall get_False(void*, BYTE* value) {
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

std::string ascii(HSTRING s) {
    UINT32 n = 0;
    const wchar_t* w = WindowsGetStringRawBuffer(s, &n);
    std::string out;
    for (UINT32 i = 0; i < n && i < kMaxLoggedChars; ++i) out += w[i] < 0x80 ? static_cast<char>(w[i]) : '?';
    return out;
}

HRESULT __stdcall GetTokenAndSignatureAsync(void*, HSTRING method, HSTRING url, HSTRING headers, void** operation) {
    if (!operation) return E_POINTER;
    static std::atomic<int> calls{0};
    if (calls++ < kLoggedTokenRequests)
        G6SHIM_INFO("User: token request %s %s (headers: %u chars) -> no network", ascii(method).c_str(),
                    ascii(url).c_str(), WindowsGetStringLen(headers));
    *operation = make_failed_async(kNetworkUnreachable);
    return S_OK;
}

HRESULT __stdcall GetTokenAndSignatureWithBodyAsync(void* self, HSTRING method, HSTRING url, HSTRING headers, void*,
                                                    void** operation) {
    return GetTokenAndSignatureAsync(self, method, url, headers, operation);
}

HRESULT __stdcall Statics_get_Value0(void*, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = kStaticsInitialValue;
    return S_OK;
}

HRESULT __stdcall get_Users(void*, void** users) {
    if (!users) return E_POINTER;
    void* one = local_user();
    *users = make_vector_view({one});
    release(one);
    return S_OK;
}

}  // namespace

void* local_user() {
    static void* one = make_probe(kUser, kNoIid);
    return add_ref(one);
}

void register_user() {
    register_interface(kDisplayInfo, kNoIid,
                       make_vtable({{display_info::kName0, fn(&get_PlayerName)},
                                    {display_info::kValue1, fn(&get_Zero32)},
                                    {display_info::kName2, fn(&get_PlayerName)},
                                    {display_info::kName3, fn(&get_PlayerName)},
                                    {display_info::kValue4, fn(&get_Zero32)},
                                    {display_info::kValue5, fn(&get_Zero32)},
                                    {display_info::kList6, fn(&get_EmptyList)}}));
    register_interface(kUser, kNoIid,
                       make_vtable({{user::kId, fn(&get_Id)},
                                    {user::kList2, fn(&get_EmptyList)},
                                    {user::kDisplayInfo3, fn(&get_DisplayInfo)},
                                    {user::kFlag4, fn(&get_False)},
                                    {user::kSignedIn5, fn(&get_False)},
                                    {user::kObject7, fn(&get_NullObject)},
                                    {user::kName8, fn(&get_PlayerName)},
                                    {user::kNumber9, fn(&get_Number)},
                                    {user::kTokenRequest, fn(&GetTokenAndSignatureAsync)},
                                    {user::kTokenRequestWithBody, fn(&GetTokenAndSignatureWithBodyAsync)}}));
    register_interface(kUser, kStatics,
                       make_vtable({{statics::kValue0, fn(&Statics_get_Value0)}, {statics::kUsers, fn(&get_Users)}}));
}

}  // namespace g6shim::winrt
