// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/winrt/activation.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/package.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <g6shim/log.hpp>

#include <array>
#include <cstdio>
#include <string_view>

namespace g6shim::winrt {

namespace {

constexpr std::array<std::string_view, 2> kConsoleNamespaces = {"Windows.Xbox.", "Windows.Kinect."};

constexpr std::array<std::string_view, 4> kOwnedClasses = {
    "Windows.ApplicationModel.Core.CoreApplication", "Windows.UI.Core.CoreWindow",
    "Windows.Devices.Input.MouseDevice", "Windows.Devices.Input.MouseCapabilities"};

constexpr std::size_t kMaxClassNameUtf8 = 512;

std::string utf8_name(const wchar_t* class_name) {
    char name[kMaxClassNameUtf8] = "?";
    if (class_name && !WideCharToMultiByte(CP_UTF8, 0, class_name, -1, name, sizeof name, nullptr, nullptr))
        lstrcpynA(name, "?", sizeof name);
    return name;
}

void log_activation(const std::string& name, const std::string& iid, const char* outcome) {
    log_once(name + iid + outcome, "activate %s %s -> %s", name.c_str(), iid.c_str(), outcome);
}

}  // namespace

bool owns_class(const std::string& class_name) {
    for (auto owned : kOwnedClasses)
        if (class_name == owned) return true;
    return false;
}

bool console_namespace(const std::string& class_name) {
    for (auto prefix : kConsoleNamespaces)
        if (class_name.rfind(prefix, 0) == 0) return true;
    return false;
}

long activate(const wchar_t* class_name, const GUID* iid, void** factory, PcActivation pc) {
    const std::string name = utf8_name(class_name);
    const std::string iid_text = iid ? guid_text(*iid) : "{?}";
    register_standins();

    if (owns_class(name) && iid && factory) {
        log_activation(name, iid_text, has_interface(name, *iid) ? "g6shim (stand-in)" : "g6shim (probe)");
        *factory = make_probe(name, *iid);
        return S_OK;
    }

    const long hr = pc ? pc(class_name, iid, factory) : E_NOTIMPL;

    bool listed = false;
    if (hr == REGDB_E_CLASSNOTREG && iid && factory) {
        const long package_hr = activate_from_package(name.c_str(), *iid, factory, listed);
        if (listed && package_hr >= 0) {
            log_activation(name, iid_text, "from the game's own DLL");
            return package_hr;
        }
    }

    const bool console_class = hr == REGDB_E_CLASSNOTREG && (console_namespace(name) || listed);
    const bool console_interface = hr == E_NOINTERFACE;
    if ((console_class || console_interface) && iid && factory) {
        if (has_interface(name, *iid)) {
            log_activation(name, iid_text, "g6shim stand-in");
        } else {
            G6SHIM_WARN("activate %s %s -> not on this PC, returning a g6shim probe", name.c_str(), iid_text.c_str());
        }
        *factory = make_probe(name, *iid);
        return S_OK;
    }
    char result[16];
    std::snprintf(result, sizeof result, "0x%08lX", static_cast<unsigned long>(hr));
    log_activation(name, iid_text, result);
    return hr;
}

}  // namespace g6shim::winrt
