# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_library(g6shim_core STATIC pe.cpp inventory.cpp hle.cpp relink.cpp)
target_include_directories(g6shim_core PUBLIC include)
