// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.hpp"

#include "pipeline.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::window {

namespace {

constexpr wchar_t kClassName[] = L"g6shim_window";
constexpr wchar_t kCaption[] = L"g6shim";
constexpr wchar_t kTitleProcProperty[] = L"g6shim.title_proc";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
constexpr WPARAM kCommandMask = 0xFFF0;
constexpr std::uint8_t kQuiesced = 1;

struct Registration {
    std::size_t id = 0;
    SuspendRoutine routine = nullptr;
    void* context = nullptr;
};

std::mutex g_routines_mutex;
std::vector<Registration> g_routines;

std::vector<Registration> suspended_routines() {
    std::lock_guard<std::mutex> lock(g_routines_mutex);
    return g_routines;
}

void finish(const char* how) {
    static std::once_flag once;
    std::call_once(once, [how] {
        G6SHIM_INFO("d3d12_xs: %s; ending the title", how);
        pipeline::save_pipeline_cache();
        if (!::TerminateProcess(::GetCurrentProcess(), 0))
            G6SHIM_ERROR("d3d12_xs: the process did not end (0x%08lX)",
                         static_cast<unsigned long>(::GetLastError()));
    });
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SYSCOMMAND && (wparam & kCommandMask) == static_cast<WPARAM>(SC_CLOSE)) {
        end_title("window closed");
        return 0;
    }
    if (message == WM_SYSCOMMAND && (wparam & kCommandMask) == static_cast<WPARAM>(SC_KEYMENU)) return 0;
    if (message == WM_CLOSE) {
        end_title("window closed");
        return 0;
    }
    const HANDLE previous = ::GetPropW(hwnd, kTitleProcProperty);
    if (message == WM_NCDESTROY) ::RemovePropW(hwnd, kTitleProcProperty);
    if (previous == nullptr) return ::DefWindowProcW(hwnd, message, wparam, lparam);
    return ::CallWindowProcW(reinterpret_cast<WNDPROC>(previous), hwnd, message, wparam, lparam);
}

void hook(HWND hwnd) {
    if (hwnd == nullptr || ::GetPropW(hwnd, kTitleProcProperty) != nullptr) return;
    const LONG_PTR previous = ::GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (previous == 0) return;
    ::SetPropW(hwnd, kTitleProcProperty, reinterpret_cast<HANDLE>(previous));
    ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc));
}

Rect rect_of(const RECT& rect) { return Rect{rect.left, rect.top, rect.right, rect.bottom}; }

Rect monitor_rect(HWND hwnd, bool work) {
    const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (monitor == nullptr || !::GetMonitorInfoW(monitor, &info)) return Rect{};
    return rect_of(work ? info.rcWork : info.rcMonitor);
}

Size client_size(HWND hwnd) {
    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return Size{};
    return Size{client.right - client.left, client.bottom - client.top};
}

void resize(HWND hwnd, std::int32_t width, std::int32_t height, std::int32_t x, std::int32_t y, UINT flags) {
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT outer{0, 0, width, height};
    ::AdjustWindowRect(&outer, static_cast<DWORD>(style), FALSE);
    ::SetWindowPos(hwnd, nullptr, x, y, outer.right - outer.left, outer.bottom - outer.top,
                   flags | SWP_NOZORDER | SWP_NOACTIVATE);
}

void apply_size(HWND hwnd, Size wanted) {
    if (hwnd == nullptr) return;
    if (fullscreen_requested()) {
        const Placement at = fullscreen_placement(monitor_rect(hwnd, false));
        if (at.client.width <= 0 || at.client.height <= 0) return;
        const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & WS_POPUP) != 0 && client_size(hwnd) == at.client) return;
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        ::SetWindowPos(hwnd, HWND_TOP, at.x, at.y, at.client.width, at.client.height,
                       SWP_FRAMECHANGED | SWP_NOACTIVATE);
        G6SHIM_INFO("d3d12_xs: window %p borderless fullscreen %dx%d", hwnd, at.client.width, at.client.height);
        return;
    }
    const Rect area = monitor_rect(hwnd, true);
    if (area.right <= area.left || area.bottom <= area.top) return;
    const std::optional<Size> forced = forced_size();
    const Placement at =
        forced ? Placement{kUsedDefault, kUsedDefault, *forced}
               : fit(area, kUsedDefault, kUsedDefault, wanted.width, wanted.height);
    if (client_size(hwnd) == at.client) return;
    if (forced) {
        RECT position{};
        ::GetWindowRect(hwnd, &position);
        resize(hwnd, at.client.width, at.client.height, position.left, position.top, 0);
    } else if (at.x == kUsedDefault || at.y == kUsedDefault) {
        resize(hwnd, at.client.width, at.client.height, 0, 0, SWP_NOMOVE);
    } else {
        resize(hwnd, at.client.width, at.client.height, at.x, at.y, 0);
    }
    const Size sized = client_size(hwnd);
    G6SHIM_INFO("d3d12_xs: window %p client %dx%d for a %dx%d render", hwnd, sized.width, sized.height,
                wanted.width, wanted.height);
}

void pump_messages() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

HWND make_window(std::uint32_t width, std::uint32_t height) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof window_class;
    window_class.lpfnWndProc = &DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = ::LoadCursorA(nullptr, IDC_ARROW);
    ::RegisterClassExW(&window_class);
    const Size wanted =
        requested_size(Size{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
    RECT outer{0, 0, wanted.width, wanted.height};
    ::AdjustWindowRect(&outer, kWindowStyle, FALSE);
    const HWND made = ::CreateWindowExW(0, kClassName, kCaption, kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                        outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr,
                                        instance, nullptr);
    if (made == nullptr) {
        G6SHIM_WARN("d3d12_xs: no presentation window (0x%08lX)",
                    static_cast<unsigned long>(::GetLastError()));
        return nullptr;
    }
    hook(made);
    apply_size(made, wanted);
    ::ShowWindow(made, SW_SHOWNORMAL);
    ::SetForegroundWindow(made);
    return made;
}

}  // namespace

void end_title(const char* why) {
    static std::atomic<bool> ending{false};
    if (ending.exchange(true)) return;
    const std::vector<Registration> routines = suspended_routines();
    G6SHIM_INFO("d3d12_xs: %s: suspending the title (%zu routines)", why, routines.size());
    std::thread([routines] {
        for (const Registration& entry : routines)
            if (entry.routine != nullptr) entry.routine(kQuiesced, entry.context);
        finish("the title suspended");
    }).detach();
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSuspendDeadlineMilliseconds));
        finish("the title did not finish suspending in time");
    }).detach();
}

std::uint32_t register_app_state_change(SuspendRoutine routine, void* context, std::size_t* registration) {
    if (routine == nullptr || registration == nullptr) return kInvalidParameter;
    static std::atomic<std::size_t> next{1};
    const std::size_t id = next.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_routines_mutex);
        g_routines.push_back(Registration{id, routine, context});
    }
    *registration = id;
    return 0;
}

void unregister_app_state_change(std::size_t registration) {
    std::lock_guard<std::mutex> lock(g_routines_mutex);
    g_routines.erase(std::remove_if(g_routines.begin(), g_routines.end(),
                                    [registration](const Registration& entry) {
                                        return entry.id == registration;
                                    }),
                     g_routines.end());
}

HWND create(std::uint32_t width, std::uint32_t height) {
    struct Shared {
        std::mutex mutex;
        std::condition_variable ready;
        HWND window = nullptr;
        bool done = false;
    };
    auto shared = std::make_shared<Shared>();
    std::thread([shared, width, height] {
        const HWND made = make_window(width, height);
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->window = made;
            shared->done = true;
        }
        shared->ready.notify_all();
        if (made != nullptr) pump_messages();
    }).detach();
    std::unique_lock<std::mutex> lock(shared->mutex);
    shared->ready.wait(lock, [&shared] { return shared->done; });
    return shared->window;
}

void adopt(HWND hwnd) {
    if (hwnd == nullptr) return;
    hook(hwnd);
    apply_size(hwnd, client_size(hwnd));
}

}  // namespace g6shim::d3d12_xs::window
