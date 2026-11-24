// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g6shim::winrt {

void* make_completed_async(void* result);
void* make_completed_action();
void* make_failed_async(long error);

}  // namespace g6shim::winrt
