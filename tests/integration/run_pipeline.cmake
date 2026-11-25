# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)

function(fail msg)
  message(FATAL_ERROR "FAIL: ${msg}")
endfunction()

function(step msg)
  message(STATUS "== ${msg}")
endfunction()

function(run out)
  execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    fail("${ARGN}\nexit ${r}\n${o}${e}")
  endif()
  set(${out} "${o}" PARENT_SCOPE)
endfunction()

function(expect text needle what)
  string(FIND "${text}" "${needle}" pos)
  if(pos EQUAL -1)
    fail("${what}: expected \"${needle}\" in:\n${text}")
  endif()
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/game" "${WORK}/hle")
file(GLOB game_files "${GAME_DIR}/*.exe" "${GAME_DIR}/*.dll")
file(COPY ${game_files} DESTINATION "${WORK}/game")

step("g6shim-scan")
run(scan "${SCAN}" "${WORK}/game")
expect("${scan}" "Needs g6shim: 5 functions in 1 DLL" "fakeconsole.dll has 5 imports")
if(NOT scan MATCHES "Ships with the game[^\n]*gamelib\\.dll")
  fail("gamelib.dll should count as part of the game:\n${scan}")
endif()
run(json "${SCAN}" "${WORK}/game" --json)
expect("${json}" "\"tool\": \"g6shim-scan\"" "JSON report names the tool")

step("g6shim-stubgen")
set(stubgen "${STUBGEN}" "${WORK}/game" -o "${WORK}/hle" --forward winmm.dll --forward kernel32.dll)
run(out ${stubgen})
expect("${out}" "3 stubs, 2 forwarders" "kernel32/winmm-named functions are forwarded, the rest stubbed")
set(def "${WORK}/hle/fakeconsole/fakeconsole.def")
if(NOT EXISTS "${def}")
  fail("no .def generated")
endif()
file(READ "${def}" text)
expect("${text}" "GetCurrentProcessId=kernel32.GetCurrentProcessId" "forwarder line")
expect("${text}" "timeGetTime=winmm.timeGetTime" "forwarder to a DLL whose import library is not linked by default")
if(EXISTS "${WORK}/hle/kernel32")
  fail("must not stub Windows DLLs")
endif()
run(out ${stubgen})
expect("${out}" "up to date" "second run changes nothing")

step("Re-adding a removed export gets a fresh stub number")
file(READ "${def}" text)
string(REGEX REPLACE "[ \t]*ConsoleGetValue=g6shim_stub_[0-9]+\r?\n" "" text "${text}")
file(WRITE "${def}" "${text}")
run(out ${stubgen})
file(READ "${def}" text)
expect("${text}" "ConsoleGetValue=g6shim_stub_3" "stub numbers 0-2 are still in the stub source")

step("Implementing ConsoleInit for real")
file(WRITE "${WORK}/hle/fakeconsole/console_init.cpp" [=[
#include <g6shim/log.hpp>

extern "C" int g6shim_ConsoleInit() {
    G6SHIM_INFO("ConsoleInit: hello from g6shim");
    return 1;
}
]=])
string(REGEX REPLACE "([ \t]*ConsoleInit)=g6shim_stub_[0-9]+" "\\1=g6shim_ConsoleInit" text "${text}")
file(WRITE "${def}" "${text}")
expect("${text}" "ConsoleInit=g6shim_ConsoleInit" "repointed ConsoleInit")
run(cov "${SCAN}" "${WORK}/game" --hle "${WORK}/hle")
expect("${cov}" "Coverage: 3/5 implemented" "coverage")

step("Building and deploying the replacement DLLs")
set(platform_args)
if(PLATFORM)
  set(platform_args -A "${PLATFORM}")
endif()
run(out "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${WORK}/build" -G "${GENERATOR}" ${platform_args}
    -DCMAKE_BUILD_TYPE=${CONFIG} -DG6SHIM_BUILD_TOOLS=OFF
    "-DG6SHIM_HLE_DIR=${WORK}/hle" "-DG6SHIM_GAME_DIR=${WORK}/game")
run(out "${CMAKE_COMMAND}" --build "${WORK}/build" --config ${CONFIG} --target g6shim_deploy)
if(NOT EXISTS "${WORK}/game/fakeconsole.dll")
  fail("fakeconsole.dll not deployed")
endif()
run(cov "${SCAN}" "${WORK}/game" --hle "${WORK}/hle")
expect("${cov}" "Coverage: 3/5 implemented" "rescan after deploy ignores g6shim's own DLLs")

step("Running the game")
set(ENV{G6SHIM_LOG_STDERR} 0)
execute_process(COMMAND "${WORK}/game/sample_game.exe" WORKING_DIRECTORY "${WORK}/game"
                OUTPUT_VARIABLE game ERROR_VARIABLE err RESULT_VARIABLE r)
message(STATUS "${game}")
if(NOT r EQUAL 0 OR NOT EXISTS "${WORK}/game/g6shim.log")
  fail("game exited with ${r}, no g6shim.log in its folder\n${err}")
endif()
file(READ "${WORK}/game/g6shim.log" log)
message(STATUS "g6shim.log:\n${log}")
expect("${game}" "ConsoleInit -> 1" "real ConsoleInit")
expect("${game}" "ConsoleGetValue(5) -> 0" "stubbed ConsoleGetValue")
expect("${game}" "GetCurrentProcessId() != 0 -> 1" "forwarded to the real kernel32 function")
expect("${game}" "timeGetTime() != 0 -> 1" "forwarded to the real winmm function")
expect("${game}" "GameLibAdd(2, 3) -> 5" "game's own DLL")
expect("${log}" "unimplemented: fakeconsole.dll!ConsoleGetValue" "stub call logged")
expect("${log}" "unimplemented: fakeconsole.dll!#7" "ordinal stub call logged")
expect("${log}" "hello from g6shim" "implemented function logged")

step("g6shim-run")
run(out "${RUN}" "${WORK}/game/sample_game.exe" --timeout 60)
message(STATUS "${out}")
if(NOT out MATCHES "load [^\n]*[\\/]game[\\/]fakeconsole\\.dll")
  fail("replacement DLL should load from the game folder:\n${out}")
endif()
expect("${out}" "debug: " "debug output captured")
expect("${out}" "hello from g6shim" "g6shim log arrives through the debugger")
expect("${out}" "exited with 0x00000000" "clean exit")
expect("${out}" "process heap compatibility -> 2" "the started game keeps the low-fragmentation heap, not the debug heap")

step("g6shim-run -p/-tid: attach to a suspended game, as for a registered package")
run(out "${SPAWN}" "${WORK}/game/sample_game.exe" "${RUN}" --timeout 60 --log "${WORK}/attach.log")
file(READ "${WORK}/attach.log" attach)
message(STATUS "${attach}")
expect("${attach}" "attached to pid" "attached")
expect("${attach}" "hello from g6shim" "debug output after attaching")
expect("${attach}" "exited with 0x00000000" "the resumed game ran to the end")

step("g6shim-relink: route the game's kernel32.dll imports through g6shim_kernel32.dll")
file(GLOB game_files "${WORK}/game/*.exe" "${WORK}/game/*.dll")
file(COPY ${game_files} DESTINATION "${WORK}/relinked")
file(REMOVE "${WORK}/relinked/sample_game.exe")
run(out "${RELINK}" "${WORK}/game/sample_game.exe" -o "${WORK}/relinked/sample_game.exe" --rename kernel32.dll)
expect("${out}" "-> g6shim_kernel32.dll" "kernel32 imports renamed")
execute_process(COMMAND "${RELINK}" "${WORK}/relinked/sample_game.exe" -o "${WORK}/relinked/sample_game.exe"
                --rename gamelib.dll RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
if(r EQUAL 0)
  fail("relink must refuse to write over its input")
endif()
run(scan "${SCAN}" "${WORK}/relinked")
expect("${scan}" "g6shim_kernel32.dll" "the relinked game needs g6shim_kernel32.dll")

run(out "${STUBGEN}" "${WORK}/relinked" -o "${WORK}/hle" --forward kernel32.dll)
expect("${out}" "g6shim_kernel32.dll: created" "pass-through DLL generated")
expect("${out}" "with 0 stubs" "every kernel32 function forwarded")

run(out "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${WORK}/build" "-DG6SHIM_GAME_DIR=${WORK}/relinked")
run(out "${CMAKE_COMMAND}" --build "${WORK}/build" --config ${CONFIG} --target g6shim_deploy)
run(scan "${SCAN}" "${WORK}/relinked")
expect("${scan}" "3 g6shim DLL(s) ignored" "deployed pass-through DLL recognised as g6shim's")

file(WRITE "${WORK}/hle/g6shim_kernel32/sleep.cpp" [=[
#include <g6shim/log.hpp>

extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);

extern "C" void g6shim_Sleep(unsigned long ms) {
    G6SHIM_INFO("Sleep(%lu) intercepted", ms);
    Sleep(ms);
}
]=])
set(kdef "${WORK}/hle/g6shim_kernel32/g6shim_kernel32.def")
file(READ "${kdef}" text)
string(REGEX REPLACE "([ \t])Sleep=kernel32\\.Sleep\r?\n" "\\1Sleep=g6shim_Sleep\n" text "${text}")
file(WRITE "${kdef}" "${text}")
expect("${text}" "Sleep=g6shim_Sleep" "repointed Sleep")

run(out "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${WORK}/build" "-DG6SHIM_GAME_DIR=${WORK}/relinked")
run(out "${CMAKE_COMMAND}" --build "${WORK}/build" --config ${CONFIG} --target g6shim_deploy)
run(out "${RUN}" "${WORK}/relinked/sample_game.exe" --timeout 60)
message(STATUS "${out}")
if(NOT out MATCHES "load [^\n]*[\\/]relinked[\\/]g6shim_kernel32\\.dll")
  fail("g6shim_kernel32.dll should load from the game folder:\n${out}")
endif()
expect("${out}" "Sleep(1) intercepted" "the relinked import reaches g6shim's function")
expect("${out}" "Sleep(1) returned" "and the real Sleep runs")
expect("${out}" "GameLibAdd(2, 3) -> 5" "everything else still works")
expect("${out}" "exited with 0x00000000" "clean exit")

step("All good")
