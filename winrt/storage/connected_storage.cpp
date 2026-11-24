// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/async.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>
#include <inspectable.h>
#include <unknwn.h>
#include <winstring.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <g6shim/log.hpp>

#include <cstdint>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace g6shim::winrt {

namespace {

namespace wf = ::winrt::Windows::Foundation;
namespace wfc = ::winrt::Windows::Foundation::Collections;
namespace wss = ::winrt::Windows::Storage::Streams;

constexpr GUID kStatics = {0x07020414, 0xB3E9, 0x47C8, {0xA3, 0x6F, 0xF2, 0x4E, 0x2D, 0xC6, 0x55, 0x73}};
constexpr char kContainer[] = "Windows.Xbox.Storage.ConnectedStorageContainer";
constexpr std::uint64_t kMaxBlob = 64ull << 20;
constexpr std::size_t kMaxNameChars = 120;
constexpr char kSpace[] = "Windows.Xbox.Storage.ConnectedStorageSpace";
constexpr std::size_t kSpaceGetForUser = 0;
constexpr std::size_t kSpaceOpenContainer = 3;
constexpr std::size_t kSpaceCreateContainer = 5;
constexpr std::size_t kContainerSpace = 1;
constexpr std::size_t kContainerSubmitUpdates = 2;
constexpr std::size_t kContainerGet = 4;
constexpr wchar_t kSavesFolder[] = L"g6shim_saves\\";
constexpr wchar_t kDefaultContainer[] = L"default";

std::string utf8(const std::wstring& w) {
    std::string out(w.size() * 3 + 1, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(),
                                      static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

std::wstring text(HSTRING s) {
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(s, &length);
    return std::wstring(raw, length);
}

std::wstring file_name(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name.substr(0, kMaxNameChars))
        out += (std::iswalnum(c) && c < 0x80) || c == L'-' || c == L'_' || c == L'.' ? c : L'_';
    if (out.empty() || out == L"." || out == L"..") out = L"_" + out;
    return out;
}

std::wstring container_dir(const std::wstring& container) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(exe, n);
    dir.erase(dir.find_last_of(L"\\/") + 1);
    dir += kSavesFolder;
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += file_name(container) + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? dir : std::wstring{};
}

std::mutex g_mutex;
std::map<const void*, std::wstring> g_containers;

void* make_container(const std::wstring& name) {
    void* container = make_probe(kContainer, kNoIid);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_containers[container] = name;
    return container;
}

std::wstring container_name(const void* container) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_containers.find(container);
    return it == g_containers.end() ? kDefaultContainer : it->second;
}

bool write_file(const std::wstring& path, const std::uint8_t* data, std::uint32_t size) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = (size == 0 || WriteFile(file, data, size, &written, nullptr)) && written == size;
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool read_file(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= kMaxBlob;
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(file);
    return ok;
}

HRESULT __stdcall GetForUserAsync(void*, void* user, void** operation) {
    if (!operation) return E_POINTER;
    G6SHIM_INFO("ConnectedStorageSpace: GetForUserAsync(user %p)", user);
    void* space = make_probe(kSpace, kNoIid);
    *operation = make_completed_async(space);
    release(space);
    return S_OK;
}

HRESULT __stdcall CreateContainer(void*, HSTRING name, void** container) {
    if (!container) return E_POINTER;
    G6SHIM_INFO("ConnectedStorageSpace: CreateContainer(\"%s\")", utf8(text(name)).c_str());
    *container = make_container(text(name));
    return S_OK;
}

HRESULT __stdcall Container_get_Space(void*, void** space) {
    if (!space) return E_POINTER;
    *space = make_probe(kSpace, kNoIid);
    return S_OK;
}

HRESULT __stdcall Space_OpenContainer(void*, HSTRING name, void** container) {
    if (!container) return E_POINTER;
    *container = make_container(text(name));
    return S_OK;
}

HRESULT __stdcall Container_SubmitUpdatesAsync(void* self, IInspectable* writes, IInspectable* deletes,
                                               void** action) {
    if (!action) return E_POINTER;
    *action = nullptr;
    const std::wstring name = container_name(self);
    const std::wstring dir = container_dir(name);
    if (dir.empty()) return E_FAIL;
    try {
        if (writes) {
            wf::IInspectable object{nullptr};
            ::winrt::copy_from_abi(object, writes);
            for (const auto& blob : object.as<wfc::IMapView<::winrt::hstring, wss::IBuffer>>()) {
                const std::wstring key(blob.Key());
                const wss::IBuffer buffer = blob.Value();
                const std::uint32_t size = buffer ? buffer.Length() : 0;
                const bool ok = write_file(dir + file_name(key), buffer ? buffer.data() : nullptr, size);
                G6SHIM_INFO("ConnectedStorage: wrote %s\\%s (%u bytes)%s", utf8(name).c_str(), utf8(key).c_str(), size,
                            ok ? "" : " FAILED");
                if (!ok) return E_FAIL;
            }
        }
        if (deletes) {
            wf::IInspectable object{nullptr};
            ::winrt::copy_from_abi(object, deletes);
            for (const auto& key : object.as<wfc::IIterable<::winrt::hstring>>()) {
                DeleteFileW((dir + file_name(std::wstring(key))).c_str());
                G6SHIM_INFO("ConnectedStorage: deleted %s\\%s", utf8(name).c_str(), utf8(std::wstring(key)).c_str());
            }
        }
    } catch (...) {
        const HRESULT hr = ::winrt::to_hresult();
        G6SHIM_WARN("ConnectedStorage: SubmitUpdatesAsync failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }
    *action = make_completed_action();
    return S_OK;
}

HRESULT __stdcall Container_GetAsync(void* self, IInspectable* names, void** operation) {
    if (!operation) return E_POINTER;
    *operation = nullptr;
    const std::wstring name = container_name(self);
    const std::wstring dir = container_dir(name);
    try {
        auto blobs = ::winrt::single_threaded_map<::winrt::hstring, wss::IBuffer>();
        if (names && !dir.empty()) {
            wf::IInspectable object{nullptr};
            ::winrt::copy_from_abi(object, names);
            for (const auto& key : object.as<wfc::IIterable<::winrt::hstring>>()) {
                std::vector<std::uint8_t> bytes;
                if (!read_file(dir + file_name(std::wstring(key)), bytes)) continue;
                wss::Buffer buffer(static_cast<std::uint32_t>(bytes.size()));
                if (!bytes.empty()) std::memcpy(buffer.data(), bytes.data(), bytes.size());
                buffer.Length(static_cast<std::uint32_t>(bytes.size()));
                blobs.Insert(key, buffer);
                G6SHIM_INFO("ConnectedStorage: read %s\\%s (%zu bytes)", utf8(name).c_str(),
                            utf8(std::wstring(key)).c_str(), bytes.size());
            }
        }
        if (blobs.Size() == 0) G6SHIM_INFO("ConnectedStorage: GetAsync in %s -> no saved data", utf8(name).c_str());
        const auto view = blobs.GetView();
        *operation = make_completed_async(::winrt::get_abi(view));
    } catch (...) {
        const HRESULT hr = ::winrt::to_hresult();
        G6SHIM_WARN("ConnectedStorage: GetAsync failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }
    return S_OK;
}

}  // namespace

void register_connected_storage() {
    register_interface(kContainer, kNoIid,
                       make_vtable({{kContainerSpace, fn(&Container_get_Space)},
                                    {kContainerSubmitUpdates, fn(&Container_SubmitUpdatesAsync)},
                                    {kContainerGet, fn(&Container_GetAsync)}}));
    register_interface(kSpace, kStatics, make_vtable({{kSpaceGetForUser, fn(&GetForUserAsync)}}));
    register_interface(kSpace, kNoIid,
                       make_vtable({{kSpaceOpenContainer, fn(&Space_OpenContainer)},
                                    {kSpaceCreateContainer, fn(&CreateContainer)}}));
}

}  // namespace g6shim::winrt
