// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/log.hpp>

#include <optional>
#include <string_view>

namespace g6shim {

enum class ConsoleModel { kSeriesX, kSeriesS };

G6SHIM_RUNTIME_API ConsoleModel console_model();

G6SHIM_RUNTIME_API void set_console_model(ConsoleModel model);

G6SHIM_RUNTIME_API std::optional<ConsoleModel> parse_console_model(std::string_view value);

G6SHIM_RUNTIME_API const char* console_model_name(ConsoleModel model);

}  // namespace g6shim
