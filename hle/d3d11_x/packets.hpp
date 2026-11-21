// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <unknwn.h>

#include <cstddef>
#include <cstdint>

namespace g6shim::d3d {

inline constexpr std::size_t kPacketFunctionField = 0xA70;

HRESULT STDMETHODCALLTYPE ContextPacket(void* self, const std::uint8_t* begin, const std::uint8_t* end);

}  // namespace g6shim::d3d
