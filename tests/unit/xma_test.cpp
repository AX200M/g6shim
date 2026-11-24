// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "xma.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace g6shim::acphal::xma;

namespace {

void put_bits(std::vector<std::uint8_t>& data, std::size_t pos, std::uint32_t count, std::uint32_t value) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = pos + i;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (7 - at % 8));
        if (((value >> (count - 1 - i)) & 1u) != 0)
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] | mask);
        else
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] & ~mask);
    }
}

std::vector<float> imdct_direct_sum(const std::vector<float>& input) {
    const std::size_t n = input.size();
    std::vector<float> out(n);
    for (std::size_t j = 0; j < n; ++j) {
        const double t = static_cast<double>(j + n / 2) + 0.5 + static_cast<double>(n) / 2.0;
        double sum = 0.0;
        for (std::size_t k = 0; k < n; ++k)
            sum += static_cast<double>(input[k]) *
                   std::cos(std::numbers::pi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
        out[j] = -static_cast<float>(sum);
    }
    return out;
}

}  // namespace

TEST(xma_zero_length_frame_before_start_ends_the_search) {
    std::vector<std::uint8_t> data(kPacketBytes, 0);
    put_bits(data, 0, 32, 2u << 26);
    const std::size_t start = 32 + 100;
    put_bits(data, start, 15, 200);
    const auto frame = read_frame(data, start);
    CHECK(frame.has_value());
    CHECK(frame->bits == 200);
    CHECK(!frame->next.has_value());
}

TEST(xma_fast_imdct_matches_the_direct_sum) {
    for (std::size_t n : {std::size_t{128}, std::size_t{256}, std::size_t{512}}) {
        std::vector<float> input(n);
        for (std::size_t i = 0; i < n; ++i) input[i] = static_cast<float>((i * 7919) % 97) - 48.0f;
        std::vector<float> fast(n, 0.0f);
        imdct_half(input.data(), n, fast.data(), 1.0f);
        const std::vector<float> direct = imdct_direct_sum(input);
        for (std::size_t i = 0; i < n; ++i)
            CHECK(std::abs(fast[i] - direct[i]) < 1e-2f * (1.0f + std::abs(direct[i])));
    }
}

TEST(xma_imdct_and_window_reconstruct_a_signal) {
    const std::size_t n = 256;
    std::vector<float> signal(n * 6);
    for (std::size_t i = 0; i < signal.size(); ++i) signal[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.3f;
    const auto window = [n](std::size_t i) {
        return std::sin((static_cast<double>(i) + 0.5) * std::numbers::pi / (2.0 * static_cast<double>(n)));
    };
    std::vector<float> out(n * 7, 0.0f);
    const std::vector<float> window_table = sine_window(n);
    for (std::size_t block = 0; block < 5; ++block) {
        const std::size_t base = block * n;
        std::vector<float> coefficients(n);
        for (std::size_t k = 0; k < n; ++k) {
            double sum = 0.0;
            for (std::size_t i = 0; i < 2 * n; ++i) {
                const double t = static_cast<double>(i) + 0.5 + static_cast<double>(n) / 2.0;
                sum += -static_cast<double>(signal[base + i]) * window(i) *
                       std::cos(std::numbers::pi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
            }
            coefficients[k] = static_cast<float>(sum);
        }
        const std::size_t at = base + n / 2;
        imdct_half(coefficients.data(), n, out.data() + at, 2.0f / static_cast<float>(n));
        if (block > 0) window_overlap(out.data() + at - n / 2, n, window_table);
    }
    for (std::size_t i = n; i < 5 * n; ++i) CHECK(std::abs(out[i] - signal[i]) < 1e-3f);
}

TEST(xma_packet_rejects_out_of_range_indices_and_partial_headers) {
    CHECK(!packet(std::vector<std::uint8_t>(4, 0), SIZE_MAX / kPacketBytes + 1).has_value());
    CHECK(!packet(std::vector<std::uint8_t>(3, 0), 0).has_value());
}

TEST(xma_packet_headers_and_frame_walk) {
    std::vector<std::uint8_t> data(2 * kPacketBytes, 0xff);
    const auto header = [](std::uint32_t frames, std::uint32_t first) {
        return (frames << 26) | (first << 11);
    };
    put_bits(data, 0, 32, header(2, 0));
    put_bits(data, kPacketBytes * 8, 32, header(1, 48));
    put_bits(data, 32, 15, 100);
    put_bits(data, 132, 15, 16300);
    const std::size_t c_start = kPacketBytes * 8 + 32 + 48;
    put_bits(data, c_start, 15, 60);

    const auto a = read_frame(data, 32);
    CHECK(a.has_value());
    CHECK(a->bits == 100);
    CHECK(a->next.has_value() && *a->next == 132);

    const auto b = read_frame(data, 132);
    CHECK(b.has_value());
    CHECK(b->bits == 16300);
    CHECK(b->next.has_value() && *b->next == c_start);
    CHECK(b->data.size() == (16300 + 7) / 8);

    const auto c = read_frame(data, c_start);
    CHECK(c.has_value());
    CHECK(c->bits == 60);
    CHECK(!c->next.has_value());
}
