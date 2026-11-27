// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "console.hpp"

#include <g6shim/console_model.hpp>

namespace g6shim::d3d12_xs {

void set_console(Console console) {
    set_console_model(console == Console::kSeriesS ? ConsoleModel::kSeriesS : ConsoleModel::kSeriesX);
}

Console current_console() {
    return console_model() == ConsoleModel::kSeriesS ? Console::kSeriesS : Console::kSeriesX;
}

}  // namespace g6shim::d3d12_xs
