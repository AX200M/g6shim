# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
add_executable(g6shim_tests
  unit/main.cpp
  unit/sample_pe.cpp
  unit/pe_test.cpp
  unit/def_test.cpp
  unit/inventory_test.cpp
  unit/relink_test.cpp
  unit/auto_event_test.cpp
  unit/manifest_test.cpp)
target_link_libraries(g6shim_tests PRIVATE g6shim_core g6shim::winrt)
target_include_directories(g6shim_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/hle/kernelx")
add_test(NAME unit COMMAND g6shim_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 30
  ENVIRONMENT "G6SHIM_LOG=${CMAKE_CURRENT_BINARY_DIR}/unit.log;G6SHIM_LOG_STDERR=0")

set(sample "${CMAKE_CURRENT_SOURCE_DIR}/integration/sample")
set(fake_lib "${CMAKE_CURRENT_BINARY_DIR}/fakeconsole.lib")
add_custom_command(OUTPUT "${fake_lib}"
  COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${sample}/fakeconsole.def" "/out:${fake_lib}"
  DEPENDS "${sample}/fakeconsole.def"
  VERBATIM)
add_custom_target(sample_fakeconsole_lib DEPENDS "${fake_lib}")

add_library(sample_gamelib SHARED "${sample}/gamelib.cpp")
add_executable(sample_game "${sample}/sample_game.cpp")
add_dependencies(sample_game sample_fakeconsole_lib)
target_link_libraries(sample_game PRIVATE sample_gamelib "${fake_lib}")
set_target_properties(sample_gamelib sample_game PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/sample_game")
set_target_properties(sample_gamelib PROPERTIES OUTPUT_NAME gamelib)

add_executable(spawn_suspended integration/spawn_suspended.cpp)

add_test(NAME integration
  COMMAND "${CMAKE_COMMAND}"
    "-DSCAN=$<TARGET_FILE:g6shim-scan>"
    "-DSTUBGEN=$<TARGET_FILE:g6shim-stubgen>"
    "-DRUN=$<TARGET_FILE:g6shim-run>"
    "-DRELINK=$<TARGET_FILE:g6shim-relink>"
    "-DSPAWN=$<TARGET_FILE:spawn_suspended>"
    "-DGAME_DIR=$<TARGET_FILE_DIR:sample_game>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK=${CMAKE_BINARY_DIR}/integration/$<CONFIG>"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DCONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_pipeline.cmake")
set_tests_properties(integration PROPERTIES TIMEOUT 30)
