// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <cstddef>

namespace g6shim {

using TitleMemoryRemapObserver = void (*)(const void* at, std::size_t bytes);

G6SHIM_RUNTIME_API void set_title_memory_remap_observer(TitleMemoryRemapObserver observer);

G6SHIM_RUNTIME_API void title_memory_remapped(const void* at, std::size_t bytes);

}  // namespace g6shim
