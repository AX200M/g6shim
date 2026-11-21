# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
add_executable(g6shim_tests
  unit/main.cpp
  unit/sample_pe.cpp
  unit/pe_test.cpp
  unit/inventory_test.cpp)
target_link_libraries(g6shim_tests PRIVATE g6shim_core)
add_test(NAME unit COMMAND g6shim_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 30
  ENVIRONMENT "G6SHIM_LOG=${CMAKE_CURRENT_BINARY_DIR}/unit.log;G6SHIM_LOG_STDERR=0")
