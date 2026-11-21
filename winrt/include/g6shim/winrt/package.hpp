// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <guiddef.h>

#include <string_view>

namespace g6shim::winrt {

bool is_safe_package_path(std::string_view path);

long activate_from_package(const char* class_name, const GUID& iid, void** factory, bool& listed);

}  // namespace g6shim::winrt
