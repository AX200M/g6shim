// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "runtime.hpp"
#include "xasync.hpp"
#include "xuser.hpp"

#include <windows.h>

#include <cstdint>
#include <string>

namespace {

namespace xgr = g6shim::xgameruntime;

std::wstring exe_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

struct Module {
    explicit Module(const wchar_t* name) : handle(LoadLibraryW((exe_folder() + L"\\" + name).c_str())) {}
    ~Module() {
        if (handle != nullptr) FreeLibrary(handle);
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    template <class Function>
    Function get(const char* name) const {
        return handle != nullptr ? reinterpret_cast<Function>(GetProcAddress(handle, name)) : nullptr;
    }
    HMODULE handle;
};

using LoadLibraryExWFunction = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryExAFunction = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryWFunction = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryAFunction = HMODULE(WINAPI*)(LPCSTR);
using QueryApiImplFunction = std::int32_t(WINAPI*)(const xgr::Guid*, const xgr::Guid*, void**);
using InitializeExFunction = std::int32_t(WINAPI*)(std::uint64_t, std::uint64_t, std::uint64_t);
using ErrorReportFunction = void(WINAPI*)(std::int32_t, const char*);

}  // namespace

TEST(xgameruntime_query_api_answers_implemented_areas_and_leaves_the_rest) {
    const xgr::Guid unknown = xgr::guid(1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4);
    void* out = reinterpret_cast<void*>(std::uintptr_t{0x1234});
    CHECK(xgr::query_api(&unknown, &unknown, &out) == xgr::kNotSupported);
    CHECK(out == reinterpret_cast<void*>(std::uintptr_t{0x1234}));
    CHECK(xgr::query_api(nullptr, nullptr, &out) == xgr::kNotSupported);

    CHECK(xgr::query_api(&xgr::xasync::kProvider, &xgr::xasync::kProvider, &out) == xgr::kOk);
    CHECK(out != nullptr);
    out = nullptr;
    CHECK(xgr::query_api(&xgr::xuser::kProvider, &xgr::xuser::kIid, &out) == xgr::kOk);
    CHECK(out != nullptr);
    CHECK(xgr::query_api(&xgr::xuser::kProvider, &xgr::xuser::kIid, nullptr) == xgr::kNotSupported);
}

TEST(xgameruntime_library_loader_forwarder_serves_the_runtime) {
    const Module libloader(L"g6shim_libloader.dll");
    CHECK(libloader.handle != nullptr);
    const LoadLibraryExWFunction load_ex = libloader.get<LoadLibraryExWFunction>("LoadLibraryExW");
    const LoadLibraryExAFunction load_ex_a = libloader.get<LoadLibraryExAFunction>("LoadLibraryExA");
    CHECK(load_ex != nullptr);
    CHECK(load_ex_a != nullptr);
    if (load_ex == nullptr || load_ex_a == nullptr) return;

    HMODULE runtime = load_ex(L"xgameruntime.dll", nullptr, 0x800);
    CHECK(runtime != nullptr);
    CHECK(runtime == GetModuleHandleW(L"g6shim_xgameruntime.dll"));
    CHECK(load_ex(L"XGAMERUNTIME", nullptr, 0) == runtime);
    CHECK(load_ex(L"C:\\nowhere\\XGameRuntime.DLL", nullptr, 0) == runtime);
    CHECK(load_ex_a("xGaMeRuNtImE.DlL", nullptr, 0) == runtime);
    CHECK(load_ex(L"my_xgameruntime.dll", nullptr, 0) == nullptr);
    CHECK(load_ex(L"xgameruntime2.dll", nullptr, 0) == nullptr);
    CHECK(load_ex(L"kernel32.dll", nullptr, 0) != nullptr);

    if (runtime == nullptr) return;
    const QueryApiImplFunction query = reinterpret_cast<QueryApiImplFunction>(GetProcAddress(runtime, "QueryApiImpl"));
    const InitializeExFunction initialize = reinterpret_cast<InitializeExFunction>(GetProcAddress(runtime, "InitializeApiImplEx"));
    const ErrorReportFunction report = reinterpret_cast<ErrorReportFunction>(GetProcAddress(runtime, "XErrorReport"));
    CHECK(query != nullptr);
    CHECK(initialize != nullptr);
    CHECK(report != nullptr);
    CHECK(GetProcAddress(runtime, "NotAnExport") == nullptr);
    if (initialize != nullptr) CHECK(initialize(1, 2, 2) == xgr::kOk);
    if (report != nullptr) report(static_cast<std::int32_t>(0x80070057u), "test");
    void* area = nullptr;
    if (query != nullptr) CHECK(query(&xgr::xuser::kProvider, &xgr::xuser::kIid, &area) == xgr::kOk);
    CHECK(area != nullptr);
}

TEST(xgameruntime_library_loader2_forwarder_serves_the_runtime) {
    const Module libloader(L"g6shim_libloader2.dll");
    CHECK(libloader.handle != nullptr);
    const LoadLibraryWFunction load = libloader.get<LoadLibraryWFunction>("LoadLibraryW");
    const LoadLibraryAFunction load_a = libloader.get<LoadLibraryAFunction>("LoadLibraryA");
    CHECK(load != nullptr);
    CHECK(load_a != nullptr);
    if (load == nullptr || load_a == nullptr) return;

    const HMODULE runtime = load(L"xgameruntime.dll");
    CHECK(runtime != nullptr);
    CHECK(runtime == GetModuleHandleW(L"g6shim_xgameruntime.dll"));
    CHECK(load(L"XGAMERUNTIME.DLL") == runtime);
    CHECK(load_a("xgameruntime") == runtime);
    CHECK(load(L"kernel32.dll") != nullptr);
    CHECK(GetProcAddress(runtime, "QueryApiImpl") != nullptr);
}
