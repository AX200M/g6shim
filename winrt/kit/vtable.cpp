// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/winrt/object.hpp>

#include "probe_object.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>

namespace g6shim::winrt {

std::string guid_text(const GUID& g) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

const void* const* make_vtable(Methods methods) {
    auto* t = new std::array<const void*, kMaxSlots>();
    const void* const* probe = probe_vtable();
    std::copy(probe, probe + kMaxSlots, t->begin());
    for (const auto& [index, method] : methods)
        if (kInspectableSlots + index < kMaxSlots) (*t)[kInspectableSlots + index] = method;
    return t->data();
}

void* make_object(const std::string& class_name, const GUID& iid, const void* const* vtable) {
    auto* p = new Probe{vtable, {}, class_name, iid};
    p->refs = 1;
    return p;
}

void log_once(const std::string& key, const char* fmt, ...) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        first = seen.insert(key).second;
    }
    if (!first && !trace_enabled()) return;
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    log(first ? Level::Info : Level::Trace, "%s", message);
}

}  // namespace g6shim::winrt
