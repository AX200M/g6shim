// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <guiddef.h>

#include <string>

namespace g6shim::winrt {

void register_interface(const std::string& class_name, const GUID& iid, const void* const* vtable);
bool has_interface(const std::string& class_name, const GUID& iid);
const void* const* registered_vtable(const std::string& class_name, const GUID& iid);

}  // namespace g6shim::winrt
