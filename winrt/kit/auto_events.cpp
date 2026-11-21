// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/events.hpp>

#include <windows.h>

namespace g6shim::winrt {

namespace {

constexpr std::uintptr_t kLowestUserAddress = 0x10000;

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

bool committed(std::uintptr_t address, DWORD access_mask) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi)) return false;
    return mbi.State == MEM_COMMIT && (mbi.Protect & access_mask) && !(mbi.Protect & PAGE_GUARD);
}

bool pointer_to(std::uintptr_t address, DWORD access_mask) {
    return address >= kLowestUserAddress && address % sizeof(void*) == 0 && committed(address, access_mask) &&
           committed(address + sizeof(void*) - 1, access_mask);
}

bool is_com_object(std::uintptr_t value) {
    if (!pointer_to(value, kReadable)) return false;
    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(value);
    if (!pointer_to(vtable, kReadable)) return false;
    const auto first = *reinterpret_cast<const std::uintptr_t*>(vtable);
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(first), &module) != 0;
}

}  // namespace

EventCall classify_event_call(const EventCallShape& shape) {
    if (shape.first_is_object && shape.second_is_writable) return EventCall::Add;
    if (shape.first_is_token) return EventCall::Remove;
    if (shape.first_is_zero && !shape.second_is_writable) return EventCall::RemoveNeverAdded;
    return EventCall::None;
}

EventCallShape event_call_shape(std::uintptr_t a, std::uintptr_t b) {
    EventCallShape shape;
    shape.first_is_object = is_com_object(a);
    shape.second_is_writable = pointer_to(b, kWritable);
    shape.first_is_token = is_event_token(static_cast<std::int64_t>(a));
    shape.first_is_zero = a == 0;
    return shape;
}

const char* event_call_name(EventCall call) {
    switch (call) {
        case EventCall::Add: return "add";
        case EventCall::Remove: return "remove";
        case EventCall::RemoveNeverAdded: return "remove (never subscribed)";
        default: return "none";
    }
}

EventCall auto_event(void* self, std::size_t method, std::uintptr_t a, std::uintptr_t b, long& result) {
    const EventCall call = classify_event_call(event_call_shape(a, b));
    switch (call) {
        case EventCall::Add:
            result = add_event_handler(self, method, reinterpret_cast<void*>(a), reinterpret_cast<std::int64_t*>(b));
            break;
        case EventCall::Remove: result = remove_event_handler(static_cast<std::int64_t>(a)); break;
        case EventCall::RemoveNeverAdded: result = S_OK; break;
        default: break;
    }
    return call;
}

}  // namespace g6shim::winrt
