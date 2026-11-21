// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "standins.hpp"

#include <g6shim/winrt/activation.hpp>

#include <mutex>

namespace g6shim::winrt {

void register_standins() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_core_application();
        register_app_model();
        register_controller();
        register_mouse();
        register_user();
        register_account_picker();
        register_application_context();
    });
}

}  // namespace g6shim::winrt
