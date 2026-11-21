// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <windows.h>

#include <g6shim/log.hpp>

extern "C" HRESULT WINAPI g6shim_AcpHalCreate(void** hal) {
    if (hal) *hal = nullptr;
    G6SHIM_WARN("AcpHalCreate: no audio control processor -> E_NOTIMPL");
    return E_NOTIMPL;
}
