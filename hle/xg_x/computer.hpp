// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "layout.hpp"

#include <atomic>
#include <cstddef>

namespace g6shim::xg {

namespace computer_slot {
inline constexpr std::size_t kAddRef = 0;
inline constexpr std::size_t kRelease = 1;
inline constexpr std::size_t kGetResourceLayout = 2;
}  // namespace computer_slot

inline constexpr std::size_t kComputerSlots = 16;

struct TextureComputer {
    const void* const* table;
    std::atomic<ULONG> refs;
    ResourceLayout layout;
};

HRESULT create_texture_2d_computer(const Texture2DDesc* desc, TextureComputer** out);

}  // namespace g6shim::xg
