// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <crtdbg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace g6shim::test {

namespace {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

int g_checks = 0;
int g_failures = 0;

}  // namespace

void add(const char* name, void (*fn)()) { cases().push_back({name, fn}); }

void count_check() { ++g_checks; }

void fail(const char* file, int line, const char* expression) {
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
    ++g_failures;
}

}  // namespace g6shim::test

int main(int argc, char** argv) {
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    using namespace g6shim::test;
    extern void g6shim_completion_tests_anchor();
    extern void g6shim_retirement_tests_anchor();
    g6shim_completion_tests_anchor();
    g6shim_retirement_tests_anchor();
    const char* only = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& c : cases()) {
        if (only && std::strstr(c.name, only) == nullptr) continue;
        c.fn();
        ++run;
    }
    std::printf("%d tests, %d checks, %d failed\n", run, g_checks, g_failures);
    return g_failures == 0 && run > 0 ? 0 : 1;
}
