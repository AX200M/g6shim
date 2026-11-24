// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/async.hpp>
#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>
#include <inspectable.h>
#include <winstring.h>

#include <g6shim/log.hpp>
#include <g6shim/window.hpp>

#include <cstdint>
#include <iterator>
#include <mutex>
#include <vector>

namespace g6shim::winrt {

namespace {

constexpr GUID kICoreApplication = {0x0AACF7A4, 0x5E1D, 0x49DF, {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
constexpr GUID kICoreApplicationView = {0x638BB2DB, 0x451D, 0x4661, {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
constexpr GUID kICoreWindow = {0x79B9D5F2, 0x879E, 0x4B89, {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
constexpr GUID kICoreDispatcher = {0x60DB2FA8, 0xB705, 0x4FDE, {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
constexpr GUID kICoreAcceleratorKeys = {0x9FFDF7F5, 0xB8C9, 0x4EF0, {0xB7, 0xD2, 0x1D, 0xE6, 0x26, 0x56, 0x1F, 0xC8}};
constexpr GUID kICoreImmersiveApplication = {0x1ADA0E3E, 0xE4A2, 0x4123, {0xB4, 0x51, 0xDC, 0x96, 0xBF, 0x80, 0x04, 0x19}};
constexpr GUID kICoreWindowStatic = {0x4D239005, 0x3C2A, 0x41B1, {0x90, 0x22, 0x53, 0x6B, 0xB9, 0xCF, 0x93, 0xB1}};
constexpr GUID kIActivatedEventArgs = {0xCF651713, 0xCD08, 0x4FD8, {0xB6, 0x97, 0xA2, 0x81, 0xB6, 0x54, 0x4E, 0x2E}};
constexpr GUID kICoreApplicationConsoleSettings = {0x42A6A529, 0x3CC6, 0x4867, {0xB5, 0xB7, 0x6D, 0x1F, 0xAF, 0x53, 0x25, 0xF9}};
constexpr GUID kILaunchActivatedEventArgs = {0xFBC93E26, 0xA14A, 0x4B4F, {0x82, 0xB0, 0x33, 0xBE, 0xD9, 0x20, 0xAF, 0x52}};

constexpr char kApplication[] = "Windows.ApplicationModel.Core.CoreApplication";
constexpr char kApplicationView[] = "Windows.ApplicationModel.Core.CoreApplicationView";
constexpr char kWindow[] = "Windows.UI.Core.CoreWindow";
constexpr char kDispatcher[] = "Windows.UI.Core.CoreDispatcher";
constexpr char kLaunchArgs[] = "Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";

namespace application {
constexpr std::size_t kAddSuspending = 1;
constexpr std::size_t kRemoveSuspending = 2;
constexpr std::size_t kAddResuming = 3;
constexpr std::size_t kRemoveResuming = 4;
constexpr std::size_t kGetCurrentView = 6;
constexpr std::size_t kRun = 7;
constexpr std::size_t kConsoleSetting = 1;
constexpr std::size_t kMainView = 0;
constexpr std::size_t kMainViewAlias = 2;
}  // namespace application

namespace view {
constexpr std::size_t kCoreWindowOrGetAt = 0;
constexpr std::size_t kAddActivated = 1;
constexpr std::size_t kRemoveActivated = 2;
}  // namespace view

namespace window {
constexpr std::size_t kGetDispatcher = 1;
constexpr std::size_t kActivate = 5;
constexpr std::size_t kGetForCurrentThread = 0;
}  // namespace window

namespace dispatcher {
constexpr std::size_t kHasThreadAccess = 0;
constexpr std::size_t kProcessEvents = 1;
constexpr std::size_t kRunAsync = 2;
constexpr int kProcessUntilQuit = 2;
}  // namespace dispatcher

namespace launch {
constexpr std::size_t kKind = 0;
constexpr std::size_t kPreviousExecutionState = 1;
constexpr std::size_t kArguments = 0;
constexpr std::size_t kTileId = 1;
constexpr int kActivationKindLaunch = 0;
constexpr int kExecutionStateNotRunning = 0;
constexpr wchar_t kTileIdValue[] = L"App";
}  // namespace launch

namespace framework_view {
constexpr std::size_t kCreateView = 6;
constexpr std::size_t kInitialize = 6;
constexpr std::size_t kSetWindow = 7;
constexpr std::size_t kLoad = 8;
constexpr std::size_t kRun = 9;
constexpr std::size_t kUninitialize = 10;
}  // namespace framework_view

constexpr std::uintptr_t kLargestIndex = 0xFFFF;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr wchar_t kWindowClass[] = L"g6shim.CoreWindow";

struct AppState {
    HWND hwnd = nullptr;
    bool closed = false;
    void* application_view = nullptr;
    void* window = nullptr;
    void* dispatcher = nullptr;
    DWORD ui_thread = 0;
} g_app;

std::mutex g_queue_mutex;
std::vector<IUnknown*> g_queue;

void run_queued() {
    std::vector<IUnknown*> work;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        work.swap(g_queue);
    }
    for (IUnknown* handler : work) {
        const HRESULT hr = vcall(handler, kDelegateInvokeSlot);
        if (FAILED(hr)) G6SHIM_WARN("dispatched callback returned 0x%08lX", static_cast<unsigned long>(hr));
        handler->Release();
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            G6SHIM_INFO("window closed by the user: ending the game");
            ExitProcess(0);
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT r{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t title[MAX_PATH] = L"g6shim";
    GetModuleFileNameW(nullptr, title, MAX_PATH);
    const wchar_t* name = title;
    for (const wchar_t* p = title; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNORMAL);
    return hwnd;
}

HRESULT __stdcall get_CoreWindow(void*, void** out) {
    if (!out) return E_POINTER;
    *out = add_ref(g_app.window);
    return S_OK;
}

HRESULT __stdcall View_CoreWindowOrGetAt(void* self, std::uintptr_t a, void** b) {
    if (a <= kLargestIndex && b) {
        if (a != 0) return E_BOUNDS;
        *b = add_ref(self);
        return S_OK;
    }
    return get_CoreWindow(self, reinterpret_cast<void**>(a));
}

HRESULT __stdcall Activate(void*) {
    if (g_app.hwnd) {
        ShowWindow(g_app.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_app.hwnd);
    }
    G6SHIM_INFO("CoreWindow.Activate");
    return S_OK;
}

HRESULT __stdcall get_Dispatcher(void*, void** out) {
    if (!out) return E_POINTER;
    if (!g_app.dispatcher) g_app.dispatcher = make_probe(kDispatcher, kICoreDispatcher);
    *out = add_ref(g_app.dispatcher);
    return S_OK;
}

void pump_messages(bool until_quit) {
    MSG msg;
    if (until_quit) {
        while (!g_app.closed && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return;
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

HRESULT __stdcall ProcessEvents(void*, int option) {
    g_app.ui_thread = GetCurrentThreadId();
    run_queued();
    pump_messages(option == dispatcher::kProcessUntilQuit);
    return S_OK;
}

HRESULT __stdcall get_HasThreadAccess(void*, BYTE* access) {
    if (!access) return E_POINTER;
    *access = g_app.ui_thread == GetCurrentThreadId() ? 1 : 0;
    return S_OK;
}

HRESULT __stdcall RunAsync(void*, int, IUnknown* handler, void** action) {
    if (!handler || !action) return E_INVALIDARG;
    handler->AddRef();
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(handler);
    }
    *action = make_completed_action();
    return S_OK;
}

HRESULT __stdcall get_MainView(void*, void** out) {
    if (!out) return E_POINTER;
    *out = add_ref(g_app.application_view);
    return S_OK;
}

HRESULT __stdcall GetForCurrentThread(void*, void** out) { return get_CoreWindow(nullptr, out); }

HRESULT __stdcall get_Kind(void*, int* kind) {
    if (!kind) return E_POINTER;
    *kind = launch::kActivationKindLaunch;
    return S_OK;
}

HRESULT __stdcall get_PreviousExecutionState(void*, int* state) {
    if (!state) return E_POINTER;
    *state = launch::kExecutionStateNotRunning;
    return S_OK;
}

HRESULT __stdcall get_Arguments(void*, HSTRING* arguments) {
    if (!arguments) return E_POINTER;
    *arguments = nullptr;
    return S_OK;
}

HRESULT __stdcall get_TileId(void*, HSTRING* tile_id) {
    if (!tile_id) return E_POINTER;
    return WindowsCreateString(launch::kTileIdValue, static_cast<UINT32>(std::size(launch::kTileIdValue) - 1), tile_id);
}

HRESULT __stdcall put_ConsoleSetting(void*, std::uint8_t value) {
    G6SHIM_INFO("CoreApplication: console setting (statics {42A6A529...} method 1) = %u, accepted", value);
    return S_OK;
}

HRESULT __stdcall Run(void*, IInspectable* view_source) {
    if (!view_source) return E_INVALIDARG;
    G6SHIM_INFO("CoreApplication.Run: running the game's view on a g6shim window");

    void* view = nullptr;
    HRESULT hr = vcall(view_source, framework_view::kCreateView, &view);
    if (FAILED(hr) || !view) {
        G6SHIM_ERROR("CreateView failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    g_app.hwnd = create_window();
    g6shim::set_main_window(g_app.hwnd);
    g_app.application_view = make_probe(kApplicationView, kICoreApplicationView);
    g_app.window = make_probe(kWindow, kICoreWindow);

    hr = vcall(view, framework_view::kInitialize, g_app.application_view);
    G6SHIM_INFO("view.Initialize -> 0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        hr = vcall(view, framework_view::kSetWindow, g_app.window);
        G6SHIM_INFO("view.SetWindow -> 0x%08lX", static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = vcall(view, framework_view::kLoad, static_cast<HSTRING>(nullptr));
        G6SHIM_INFO("view.Load -> 0x%08lX", static_cast<unsigned long>(hr));
    }
    if (SUCCEEDED(hr)) {
        void* args = make_probe(kLaunchArgs, kIActivatedEventArgs);
        const std::size_t n = raise_event(g_app.application_view, view::kAddActivated, g_app.application_view, args);
        G6SHIM_INFO("raised CoreApplicationView.Activated (%zu handler(s))", n);
        release(args);
    }
    if (SUCCEEDED(hr)) {
        hr = vcall(view, framework_view::kRun);
        G6SHIM_INFO("view.Run -> 0x%08lX", static_cast<unsigned long>(hr));
    }
    vcall(view, framework_view::kUninitialize);
    release(view);
    return hr;
}

}  // namespace

void register_app_model() {
    register_interface(kApplication, kICoreApplication,
                       make_vtable({{application::kAddSuspending, event_add(application::kAddSuspending)},
                                    {application::kRemoveSuspending, event_remove()},
                                    {application::kAddResuming, event_add(application::kAddResuming)},
                                    {application::kRemoveResuming, event_remove()},
                                    {application::kGetCurrentView, fn(&get_MainView)},
                                    {application::kRun, fn(&Run)}}));

    register_interface(kApplicationView, kICoreApplicationView,
                       make_vtable({{view::kCoreWindowOrGetAt, fn(&View_CoreWindowOrGetAt)},
                                    {view::kAddActivated, event_add(view::kAddActivated)},
                                    {view::kRemoveActivated, event_remove()}}));

    register_interface(kWindow, kICoreWindow,
                       make_vtable({{window::kGetDispatcher, fn(&get_Dispatcher)}, {window::kActivate, fn(&Activate)}}));

    register_interface(kDispatcher, kICoreDispatcher,
                       make_vtable({{dispatcher::kHasThreadAccess, fn(&get_HasThreadAccess)},
                                    {dispatcher::kProcessEvents, fn(&ProcessEvents)},
                                    {dispatcher::kRunAsync, fn(&RunAsync)}}));

    register_interface(kDispatcher, kICoreAcceleratorKeys, make_vtable({}));

    register_interface(kApplication, kICoreApplicationConsoleSettings,
                       make_vtable({{application::kConsoleSetting, fn(&put_ConsoleSetting)}}));

    register_interface(kApplication, kICoreImmersiveApplication,
                       make_vtable({{application::kMainView, fn(&get_MainView)},
                                    {application::kMainViewAlias, fn(&get_MainView)}}));

    register_interface(kWindow, kICoreWindowStatic, make_vtable({{window::kGetForCurrentThread, fn(&GetForCurrentThread)}}));

    register_interface(kLaunchArgs, kIActivatedEventArgs,
                       make_vtable({{launch::kKind, fn(&get_Kind)},
                                    {launch::kPreviousExecutionState, fn(&get_PreviousExecutionState)}}));
    register_interface(kLaunchArgs, kILaunchActivatedEventArgs,
                       make_vtable({{launch::kArguments, fn(&get_Arguments)}, {launch::kTileId, fn(&get_TileId)}}));
}

}  // namespace g6shim::winrt
