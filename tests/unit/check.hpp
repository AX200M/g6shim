// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g6shim::test {

void add(const char* name, void (*fn)());
void count_check();
void fail(const char* file, int line, const char* expression);

struct Registrar {
    Registrar(const char* name, void (*fn)()) { add(name, fn); }
};

}  // namespace g6shim::test

#define CHECK(cond)                                                             \
    do {                                                                        \
        ::g6shim::test::count_check();                                          \
        __pragma(warning(push))                                                 \
        __pragma(warning(disable : 4127))                                       \
        if (!(cond)) ::g6shim::test::fail(__FILE__, __LINE__, #cond);           \
        __pragma(warning(pop))                                                  \
    } while (0)

#define TEST(name)                                                              \
    static void name();                                                         \
    static const ::g6shim::test::Registrar name##_registrar{#name, &name};      \
    static void name()

#ifdef _MSC_VER
#pragma warning(disable : 4127)
#endif
