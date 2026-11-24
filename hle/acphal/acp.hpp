// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>

namespace g6shim::acphal {

struct Acp;

struct Message {
    std::uint32_t kind;
    std::uint32_t dropped;
    std::uint32_t usec;
    std::uint8_t data[16];
};

static_assert(sizeof(Message) == 28);

struct AcpMethods {
    HRESULT(WINAPI* connect)(Acp*, std::uint32_t, std::uint32_t);
    HRESULT(WINAPI* disconnect)(Acp*);
    HRESULT(WINAPI* submit)(Acp*, std::uint32_t, std::uint64_t, std::uint32_t, const void*, std::uint32_t);
    bool(WINAPI* pop_message)(Acp*, Message*);
    std::uint32_t(WINAPI* message_count)(Acp*);
    void(WINAPI* release)(Acp*);
};

struct Acp {
    const AcpMethods* methods;
};

HRESULT create_acp(Acp** out);

}  // namespace g6shim::acphal
