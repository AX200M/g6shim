// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace g6shim::acphal {

void set_xma_context_array(std::uint8_t* array, std::uint32_t voices);
void clear_xma_context_array();
std::uint32_t xma_voice_count();
void xma_enable(std::uint32_t index);
void xma_disable(std::uint32_t index);
void xma_advance_read(std::uint32_t index, std::uint32_t blocks);
void xma_pump();
void decoder_wait(void* timer, void* wake);
std::optional<std::size_t> xma_produced(std::uint32_t index);

}  // namespace g6shim::acphal
