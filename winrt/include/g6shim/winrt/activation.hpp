// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <guiddef.h>

#include <string>

namespace g6shim::winrt {

using PcActivation = long (*)(const wchar_t* class_name, const GUID* iid, void** factory);

long activate(const wchar_t* class_name, const GUID* iid, void** factory, PcActivation pc);

bool owns_class(const std::string& class_name);
bool console_namespace(const std::string& class_name);

void register_standins();

}  // namespace g6shim::winrt
