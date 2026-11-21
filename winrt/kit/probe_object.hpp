// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <guiddef.h>
#include <windows.h>

#include <atomic>
#include <string>

namespace g6shim::winrt {

struct Probe {
    const void* const* vtable;
    std::atomic<ULONG> refs{1};
    std::string class_name;
    GUID iid;
};

const void* const* probe_vtable();

}  // namespace g6shim::winrt
