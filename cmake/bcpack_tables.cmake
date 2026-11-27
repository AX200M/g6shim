# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set(G6SHIM_BCPACK_TABLES_URL
    "https://raw.githubusercontent.com/Marvisak/VIctory/2849477415a46f5a079c85cf3e64a3dff32f713c/crates/victory-formats/src/bcpack-tables.bin")
set(G6SHIM_BCPACK_TABLES_SHA256 "1f1d4760419202bb5672f0c428ce51e456b5d08fbaec8f371d043fd94e01c36b")

function(g6shim_fetch_bcpack_tables target)
  set(file "${CMAKE_BINARY_DIR}/third_party/bcpack-tables.bin")
  if(NOT EXISTS "${file}")
    file(DOWNLOAD "${G6SHIM_BCPACK_TABLES_URL}" "${file}"
         EXPECTED_HASH SHA256=${G6SHIM_BCPACK_TABLES_SHA256}
         STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${file}")
      message(FATAL_ERROR "BCPack tables could not be downloaded: ${status}")
    endif()
  endif()
  file(SHA256 "${file}" hash)
  if(NOT hash STREQUAL G6SHIM_BCPACK_TABLES_SHA256)
    file(REMOVE "${file}")
    message(FATAL_ERROR "BCPack tables have the wrong hash; reconfigure to download them again")
  endif()
  set_property(GLOBAL APPEND PROPERTY G6SHIM_DEPLOY_FILES "${file}")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "$<TARGET_FILE_DIR:${target}>/bcpack-tables.bin"
    VERBATIM)
endfunction()
