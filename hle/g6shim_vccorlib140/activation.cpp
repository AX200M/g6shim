// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/activation.hpp>

#include <windows.h>

namespace {

constexpr wchar_t kPcRuntime[] = L"vccorlib140.dll";
constexpr char kGetActivationFactory[] = "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z";

using GetFactory = long (*)(void* class_name, void* iid, void** factory);

GetFactory pc_get_factory() {
    static const GetFactory fn = [] {
        HMODULE dll = LoadLibraryW(kPcRuntime);
        return dll ? reinterpret_cast<GetFactory>(GetProcAddress(dll, kGetActivationFactory)) : nullptr;
    }();
    return fn;
}

long pc_activation(const wchar_t* class_name, const GUID* iid, void** factory) {
    const GetFactory get = pc_get_factory();
    if (!get) return E_NOTIMPL;
    return get(const_cast<wchar_t*>(class_name), const_cast<GUID*>(iid), factory);
}

}  // namespace

extern "C" long g6shim_GetActivationFactoryByPCWSTR(void* class_name, void* iid, void** factory) {
    return g6shim::winrt::activate(static_cast<const wchar_t*>(class_name), static_cast<const GUID*>(iid), factory,
                                   &pc_activation);
}
