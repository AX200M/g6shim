// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "frames.hpp"

namespace {

void usage() {
    std::fputs(
        "usage: g6shim-run <exe> [--timeout <seconds>] [-v] [--log <file>] [-- <game arguments>...]\n"
        "       g6shim-run -p <pid> [-tid <tid>] [--timeout <seconds>] [-v] [--log <file>]\n"
        "\n"
        "Starts the game in its own folder under a minimal debugger (or attaches to it)\n"
        "and prints its debug output, DLLs loaded from outside System32, exceptions and\n"
        "the exit code.\n"
        "\n"
        "options:\n"
        "  --timeout <s>  stop the game after this many seconds (default 30)\n"
        "  -v             list every DLL load and every exception\n"
        "  --log <file>   write the report to a file instead of the console\n"
        "  -p, -tid       attach to a running process and resume the given thread: what\n"
        "                 Windows appends for a package debugger, e.g.\n"
        "                 plmdebug /enableDebug <package> \"g6shim-run.exe --log <file>\"\n",
        stderr);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring path_of(HANDLE file) {
    if (!file) return L"?";
    wchar_t buf[2048];
    const DWORD n = GetFinalPathNameByHandleW(file, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED);
    if (n == 0 || n >= std::size(buf)) return L"?";
    std::wstring p(buf, n);
    if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    return p;
}

std::wstring file_name(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string hresult_text(std::uint32_t hr) {
    char* buf = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, hr, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string s = n && buf ? std::string(buf, n) : std::string();
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

constexpr DWORD kCppException = 0xE06D7363;
constexpr DWORD kStowedException = 0xC000027B;
constexpr DWORD kFailFast = 0xC0000409;
constexpr DWORD kDllNotFound = 0xC0000135;
constexpr DWORD kEntryPointNotFound = 0xC0000139;
constexpr DWORD kThreadName = 0x406D1388;
constexpr DWORD kRoOriginateError = 0x40080201;
constexpr DWORD kRoTransformError = 0x40080202;
constexpr std::uint32_t kStowedSignatureV1 = 0x53453031;
constexpr std::uint32_t kStowedSignatureV2 = 0x53453032;
constexpr ULONG_PTR kMaxStowedRecords = 16;
constexpr ULONG_PTR kMaxRoErrorChars = 1024;
constexpr std::size_t kMaxDebugString = 8192;
constexpr int kQuietFirstChanceAfter = 50;
constexpr DWORD kDebugEventWaitMs = 200;

BOOL CALLBACK read_debuggee(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), buffer, size, &got))
        return FALSE;
    if (read) *read = static_cast<DWORD>(got);
    return TRUE;
}

PVOID CALLBACK function_table(HANDLE process, DWORD64 address) {
    return SymFunctionTableAccess64(process, address);
}

DWORD64 CALLBACK module_base(HANDLE process, DWORD64 address) {
    return SymGetModuleBase64(process, address);
}

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case kCppException: return "C++ exception";
        case kStowedException: return "stowed exception (unhandled WinRT error)";
        case kFailFast: return "fail-fast";
        case kDllNotFound: return "DLL not found";
        case kEntryPointNotFound: return "entry point not found";
        case kThreadName: return "thread name";
        case kRoOriginateError: return "WinRT error originated";
        case kRoTransformError: return "WinRT error transformed";
        default: return "";
    }
}

class Session {
public:
    Session(HANDLE process, bool verbose, std::wstring system_dir)
        : process_(process), verbose_(verbose), system_dir_(lower(std::move(system_dir))) {}

    ~Session() { shutdown(); }

    void set_process(HANDLE process) { process_ = process; }

    void shutdown() {
        if (symbols_) {
            SymCleanup(process_);
            symbols_ = false;
        }
    }

    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    void loaded(std::uintptr_t base, const std::wstring& path) {
        modules_[base] = path;
        refresh_symbols();
        const bool system = lower(path).rfind(system_dir_, 0) == 0;
        if (verbose_ || !system) std::printf("[%8.3f] load %s\n", seconds(), narrow(path).c_str());
    }

    void unloaded(std::uintptr_t base) {
        modules_.erase(base);
        refresh_symbols();
    }

    std::string where(std::uintptr_t address) const {
        auto it = modules_.upper_bound(address);
        if (it == modules_.begin()) return g6shim::run::format_raw_address(address);
        --it;
        return g6shim::run::format_module_address(address, it->first, narrow(file_name(it->second)));
    }

    void debug_string(const OUTPUT_DEBUG_STRING_INFO& info) {
        const std::size_t len = std::min<std::size_t>(info.nDebugStringLength, kMaxDebugString);
        std::string s(len, '\0');
        SIZE_T got = 0;
        if (!ReadProcessMemory(process_, info.lpDebugStringData, s.data(), len, &got)) return;
        s.resize(got);
        if (const auto nul = s.find('\0'); nul != std::string::npos) s.resize(nul);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) std::printf("[%8.3f] debug: %s\n", seconds(), s.c_str());
    }

    DWORD exception(const EXCEPTION_DEBUG_INFO& info, DWORD thread_id) {
        const EXCEPTION_RECORD& ex = info.ExceptionRecord;
        if (!initial_breakpoint_seen_ && ex.ExceptionCode == EXCEPTION_BREAKPOINT) {
            initial_breakpoint_seen_ = true;
            open_symbols();
            return DBG_CONTINUE;
        }
        const bool first = info.dwFirstChance != 0;
        const bool quiet = first && (ex.ExceptionCode == kThreadName || (!verbose_ && ++first_chance_ > kQuietFirstChanceAfter));
        if (!quiet) {
            std::printf("[%8.3f] exception 0x%08lX %s at %s%s\n", seconds(), ex.ExceptionCode,
                        exception_name(ex.ExceptionCode),
                        where(reinterpret_cast<std::uintptr_t>(ex.ExceptionAddress)).c_str(),
                        first ? " (first chance)" : " (unhandled)");
            if (ex.ExceptionCode == kStowedException) stowed(ex);
            if (ex.ExceptionCode == kRoOriginateError || ex.ExceptionCode == kRoTransformError) ro_error(ex);
        }
        if (!first) {
            open_symbols();
            stack(thread_id, reinterpret_cast<std::uintptr_t>(ex.ExceptionAddress));
        }
        return DBG_EXCEPTION_NOT_HANDLED;
    }

private:
    template <class T>
    bool read(std::uintptr_t address, T& out) const {
        SIZE_T got = 0;
        return ReadProcessMemory(process_, reinterpret_cast<const void*>(address), &out, sizeof out, &got) &&
               got == sizeof out;
    }

    void open_symbols() {
        if (!process_ || symbols_) return;
        SymSetOptions(SYMOPT_IGNORE_NT_SYMPATH | SYMOPT_DEFERRED_LOADS);
        symbols_ = SymInitialize(process_, nullptr, TRUE) != FALSE;
    }

    void refresh_symbols() const {
        if (symbols_) SymRefreshModuleList(process_);
    }

    void stack(DWORD thread_id, std::uintptr_t fault_address) const {
        std::printf("  #0 %s\n", where(fault_address).c_str());
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
        if (!thread) return;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        const bool have_context = GetThreadContext(thread, &context) != FALSE;
        if (have_context && symbols_ && process_) {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            g6shim::run::FrameStop stop;
            stop.keep(fault_address);
            bool current = true;
            std::size_t index = 0;
            while (index + 1 < g6shim::run::kMaxStackFrames) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process_, thread, &frame, &context, read_debuggee,
                                 function_table, module_base, nullptr))
                    break;
                const auto address = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
                if (current) {
                    current = false;
                    stop.keep(address);
                    continue;
                }
                if (!stop.keep(address)) break;
                ++index;
                std::printf("  #%zu %s\n", index, where(address).c_str());
            }
        }
        CloseHandle(thread);
    }

    void stowed(const EXCEPTION_RECORD& ex) const {
        if (ex.NumberParameters < 2) return;
        const auto array = static_cast<std::uintptr_t>(ex.ExceptionInformation[0]);
        const auto count = std::min<ULONG_PTR>(ex.ExceptionInformation[1], kMaxStowedRecords);
        for (ULONG_PTR i = 0; i < count; ++i) {
            std::uintptr_t record = 0;
            struct {
                std::uint32_t size, signature;
                std::uint32_t result;
            } head{};
            if (!read(array + i * sizeof(void*), record) || !read(record, head)) continue;
            if (head.signature != kStowedSignatureV1 && head.signature != kStowedSignatureV2) continue;
            std::printf("           stowed error 0x%08X %s\n", head.result, hresult_text(head.result).c_str());
        }
    }

    void ro_error(const EXCEPTION_RECORD& ex) const {
        if (ex.NumberParameters < 3) return;
        const auto hr = static_cast<std::uint32_t>(ex.ExceptionInformation[0]);
        const auto len = std::min<ULONG_PTR>(ex.ExceptionInformation[1], kMaxRoErrorChars);
        std::wstring msg(len, L'\0');
        SIZE_T got = 0;
        ReadProcessMemory(process_, reinterpret_cast<const void*>(ex.ExceptionInformation[2]), msg.data(),
                          len * sizeof(wchar_t), &got);
        msg.resize(got / sizeof(wchar_t));
        if (const auto nul = msg.find(L'\0'); nul != std::wstring::npos) msg.resize(nul);
        while (!msg.empty() && std::iswspace(msg.back())) msg.pop_back();
        std::printf("           WinRT error 0x%08X %s\n", hr, narrow(msg).c_str());
    }

    HANDLE process_;
    bool verbose_;
    std::wstring system_dir_;
    std::map<std::uintptr_t, std::wstring> modules_;
    bool symbols_ = false;
    bool initial_breakpoint_seen_ = false;
    int first_chance_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        out += arg[i];
    }
    return out + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring exe, log_path;
    std::vector<std::wstring> game_args;
    double timeout = 30;
    bool verbose = false;
    DWORD attach_pid = 0, resume_tid = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"-h" || a == L"--help") {
            usage();
            return 0;
        } else if (a == L"--timeout" && i + 1 < argc) {
            timeout = std::wcstod(argv[++i], nullptr);
        } else if (a == L"-v") {
            verbose = true;
        } else if (a == L"--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (a == L"-p" && i + 1 < argc) {
            attach_pid = std::wcstoul(argv[++i], nullptr, 10);
        } else if (a == L"-tid" && i + 1 < argc) {
            resume_tid = std::wcstoul(argv[++i], nullptr, 10);
        } else if (a == L"--") {
            game_args.assign(argv + i + 1, argv + argc);
            break;
        } else if (exe.empty() && !a.empty() && a[0] != L'-') {
            exe = a;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", narrow(a).c_str());
            usage();
            return 2;
        }
    }
    if ((exe.empty() == (attach_pid == 0)) || !(timeout > 0)) {
        usage();
        return 2;
    }
    if (!log_path.empty()) {
        if (!_wfreopen(log_path.c_str(), L"w", stdout)) return 1;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }

    wchar_t sysdir[MAX_PATH];
    const UINT sys_len = GetSystemDirectoryW(sysdir, MAX_PATH);
    const std::wstring system_dir = sys_len ? std::wstring(sysdir, sys_len) + L"\\" : L"";

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    HANDLE process = nullptr;
    PROCESS_INFORMATION pi{};
    if (attach_pid) {
        if (!DebugActiveProcess(attach_pid)) {
            std::printf("g6shim-run: cannot attach to pid %lu (error %lu)\n", attach_pid, GetLastError());
            return 1;
        }
        std::printf("g6shim-run: attached to pid %lu (timeout %.0f s)\n", attach_pid, timeout);
        if (resume_tid) {
            if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, resume_tid)) {
                ResumeThread(t);
                CloseHandle(t);
            }
        }
    } else {
        wchar_t full[MAX_PATH * 4];
        if (!GetFullPathNameW(exe.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr)) {
            std::fprintf(stderr, "g6shim-run: bad path\n");
            return 2;
        }
        exe = full;
        const std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
        std::wstring cmdline = quote(exe);
        for (const auto& a : game_args) cmdline += L" " + quote(a);

        STARTUPINFOW si{};
        si.cb = sizeof si;
        if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr,
                            dir.c_str(), &si, &pi)) {
            std::fprintf(stderr, "g6shim-run: cannot start %s (error %lu)\n", narrow(exe).c_str(), GetLastError());
            return 1;
        }
        process = pi.hProcess;
        std::printf("g6shim-run: %s (pid %lu, timeout %.0f s)\n", narrow(exe).c_str(), pi.dwProcessId, timeout);
    }

    Session session(process, verbose, system_dir);
    HANDLE target = process;
    DWORD exit_code = 0;
    bool stopped = false;
    for (;;) {
        if (!stopped && session.seconds() > timeout) {
            std::printf("[%8.3f] still running after %.0f s: stopping it\n", session.seconds(), timeout);
            if (target) TerminateProcess(target, 1);
            stopped = true;
        }
        DEBUG_EVENT ev{};
        if (!WaitForDebugEvent(&ev, kDebugEventWaitMs)) continue;
        DWORD status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (!target) {
                    target = ev.u.CreateProcessInfo.hProcess;
                    session.set_process(target);
                }
                session.loaded(reinterpret_cast<std::uintptr_t>(ev.u.CreateProcessInfo.lpBaseOfImage),
                               path_of(ev.u.CreateProcessInfo.hFile));
                if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                session.loaded(reinterpret_cast<std::uintptr_t>(ev.u.LoadDll.lpBaseOfDll), path_of(ev.u.LoadDll.hFile));
                if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
                break;
            case UNLOAD_DLL_DEBUG_EVENT:
                session.unloaded(reinterpret_cast<std::uintptr_t>(ev.u.UnloadDll.lpBaseOfDll));
                break;
            case OUTPUT_DEBUG_STRING_EVENT:
                session.debug_string(ev.u.DebugString);
                break;
            case EXCEPTION_DEBUG_EVENT:
                status = session.exception(ev.u.Exception, ev.dwThreadId);
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                exit_code = ev.u.ExitProcess.dwExitCode;
                break;
            default:
                break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) break;
    }

    const char* name = exception_name(exit_code);
    std::printf("[%8.3f] exited with 0x%08lX%s%s\n", session.seconds(), exit_code, *name ? " " : "", name);
    session.shutdown();
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    return exit_code == 0 && !stopped ? 0 : 1;
}
