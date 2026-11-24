// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace g6shim::acphal::xma {

inline constexpr std::size_t kPacketBytes = 2048;
inline constexpr std::size_t kFrameSamples = 512;
inline constexpr std::size_t kSubframeSamples = 128;

struct Packet {
    std::uint32_t frames = 0;
    std::uint32_t first_frame_bit = 0;
    std::uint32_t skip = 0;
};

struct Frame {
    std::vector<std::uint8_t> data;
    std::size_t bits = 0;
    std::size_t start = 0;
    std::optional<std::size_t> next;
};

std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index);
std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start);

void imdct_half(const float* input, std::size_t n, float* out, float scale);
std::vector<float> sine_window(std::size_t n);
void window_overlap(float* block, std::size_t block_len, const std::vector<float>& window);

class Decoder {
public:
    Decoder(std::size_t channels, std::uint32_t sample_rate);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::size_t channels() const;
    void reset();
    bool decode(std::span<const std::uint8_t> data, std::size_t bits, std::vector<std::int16_t>& out);
    void flush(std::vector<std::int16_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace g6shim::acphal::xma
