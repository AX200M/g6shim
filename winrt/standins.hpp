// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

namespace g6shim::winrt {

void register_core_application();
void register_app_model();
void register_controller();
void register_mouse();
void register_user();
void register_account_picker();
void register_application_context();
void register_connected_storage();

void* local_user();
const void* gamepad_get_current_reading();

}  // namespace g6shim::winrt
