// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <unknwn.h>

#include <cstddef>

namespace g6shim::d3d {

enum class Layout { Resource, View };

void use_console_layout(IUnknown* object, Layout layout);

template <std::size_t Slot, Layout L>
HRESULT STDMETHODCALLTYPE CreateWithConsoleLayout(void* self, void* a, void* b, IUnknown** out);

}  // namespace g6shim::d3d
