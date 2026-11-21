// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <vector>

namespace g6shim::winrt {

void* make_vector_view(const std::vector<void*>& items);
void* make_empty_vector_view();
void* make_vector();
void* make_empty_map_view();

}  // namespace g6shim::winrt
