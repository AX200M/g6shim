// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/log.hpp"
#include "g6shim/settings.hpp"

#include "log_format.hpp"

#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace g6shim::settings {
void hold_warnings(bool hold);
void flush_warnings();
}

namespace g6shim {

namespace {

constexpr char kLogVariable[] = "G6SHIM_LOG";
constexpr char kStderrVariable[] = "G6SHIM_LOG_STDERR";
constexpr char kTraceVariable[] = "G6SHIM_TRACE";
constexpr wchar_t kDefaultLog[] = L"g6shim.log";
constexpr std::size_t kMaxMessage = 2048;
constexpr std::size_t kMaxLine = kMaxMessage + 256;
constexpr unsigned long long kThreadTagModulus = 100000;


bool flag(const char* name, bool fallback) {
    const std::optional<std::string> value = settings::get(name);
    if (!value || value->empty()) return fallback;
    return value->front() != '0';
}

struct Sink {
    std::mutex mutex;
    std::FILE* file = nullptr;
    bool to_stderr = true;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    Sink() {
        const std::optional<std::wstring> path = settings::get_wide(kLogVariable);
        file = _wfopen(path && !path->empty() ? path->c_str() : kDefaultLog, L"a");
        to_stderr = flag(kStderrVariable, true);
    }
};

Sink& sink() {
    static Sink* s = [] {
        settings::hold_warnings(true);
        Sink* created = new Sink();
        settings::hold_warnings(false);
        settings::flush_warnings();
        return created;
    }();
    return *s;
}

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        default: return "ERROR";
    }
}

void write_line(Level level, const char* message) {
    Sink& s = sink();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                               s.start);
    const std::string timestamp = detail::format_elapsed(elapsed);
    const auto tid = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) %
                                                     kThreadTagModulus);

    char line[kMaxLine];
    std::snprintf(line, sizeof line, "[%s] [%s] [t%05llu] %s\n", timestamp.c_str(), level_name(level), tid, message);

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fputs(line, s.file);
        std::fflush(s.file);
    }
    if (s.to_stderr) std::fputs(line, stderr);
    OutputDebugStringA(line);
}

}  // namespace

void log(Level level, const char* fmt, ...) {
    char message[kMaxMessage];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    write_line(level, message);
}

void log_unimplemented(const char* dll, const char* function) {
    log(Level::Warn, "unimplemented: %s!%s", dll, function);
}

bool trace_enabled() {
    static const bool enabled = flag(kTraceVariable, false);
    return enabled;
}

}  // namespace g6shim

extern "C" void g6shim_runtime_marker() {}
