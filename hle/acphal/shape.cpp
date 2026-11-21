// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <windows.h>

#include <g6shim/log.hpp>

extern "C" HRESULT g6shim_AcpHalAllocateShapeContexts(void* a, void* b) {
    G6SHIM_INFO("AcpHalAllocateShapeContexts(%p, %p): no audio hardware -> E_FAIL", a, b);
    return E_FAIL;
}
