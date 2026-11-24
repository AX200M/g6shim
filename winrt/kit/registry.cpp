// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <map>
#include <mutex>

namespace g6shim::winrt {

namespace {

std::mutex g_mutex;

std::map<std::string, const void* const*>& registry() {
    static std::map<std::string, const void* const*> map;
    return map;
}

std::string key(const std::string& class_name, const GUID& iid) { return class_name + guid_text(iid); }

}  // namespace

void register_interface(const std::string& class_name, const GUID& iid, const void* const* vtable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    registry()[key(class_name, iid)] = vtable;
}

bool has_interface(const std::string& class_name, const GUID& iid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return registry().count(key(class_name, iid)) != 0;
}

const void* const* registered_vtable(const std::string& class_name, const GUID& iid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = registry().find(key(class_name, iid));
    return it == registry().end() ? nullptr : it->second;
}

}  // namespace g6shim::winrt
