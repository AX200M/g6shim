// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g6shim::winrt {

inline constexpr std::int64_t kFirstEventToken = 0x10001;

const void* event_add(std::size_t method);
const void* event_remove();

long add_event_handler(void* self, std::size_t method, void* handler, std::int64_t* token);
long remove_event_handler(std::int64_t token);
bool is_event_token(std::int64_t token);

std::size_t raise_event(void* object, std::size_t method, void* sender, void* args);

enum class EventCall { None, Add, Remove, RemoveNeverAdded };

struct EventCallShape {
    bool first_is_object = false;
    bool second_is_writable = false;
    bool first_is_token = false;
    bool first_is_zero = false;
};

EventCall classify_event_call(const EventCallShape& shape);

EventCallShape event_call_shape(std::uintptr_t a, std::uintptr_t b);

const char* event_call_name(EventCall call);

EventCall auto_event(void* self, std::size_t method, std::uintptr_t a, std::uintptr_t b, long& result);

}  // namespace g6shim::winrt
