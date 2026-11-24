// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(G6SHIM_RUNTIME_BUILD)
#  define G6SHIM_RUNTIME_API __declspec(dllexport)
#else
#  define G6SHIM_RUNTIME_API __declspec(dllimport)
#endif

namespace g6shim {

enum class Level { Trace, Info, Warn, Error };

G6SHIM_RUNTIME_API void log(Level level, const char* fmt, ...);

G6SHIM_RUNTIME_API void log_unimplemented(const char* dll, const char* function);

G6SHIM_RUNTIME_API bool trace_enabled();

}  // namespace g6shim

extern "C" G6SHIM_RUNTIME_API void g6shim_runtime_marker();

#define G6SHIM_TRACE(...)                                                     \
    do {                                                                      \
        if (::g6shim::trace_enabled()) ::g6shim::log(::g6shim::Level::Trace, __VA_ARGS__); \
    } while (0)
#define G6SHIM_INFO(...) ::g6shim::log(::g6shim::Level::Info, __VA_ARGS__)
#define G6SHIM_WARN(...) ::g6shim::log(::g6shim::Level::Warn, __VA_ARGS__)
#define G6SHIM_ERROR(...) ::g6shim::log(::g6shim::Level::Error, __VA_ARGS__)
