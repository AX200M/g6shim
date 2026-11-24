// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "bcpack.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include <windows.h>

using namespace g6shim::xcrdapi;

namespace {

std::vector<std::uint8_t> test_tables() {
    std::vector<std::uint8_t> bytes;
    for (std::size_t m = 0; m < kBcpackModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < kBcpackBuckets; ++b) {
            const std::uint16_t f =
                b < g + 2 ? static_cast<std::uint16_t>(1u << (15 - g)) : std::uint16_t{0};
            bytes.push_back(static_cast<std::uint8_t>(f & 0xff));
            bytes.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v & 0x77));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v <= 255; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned v = 0; v < 64; ++v)
        bytes.push_back(static_cast<std::uint8_t>((v & 7) | ((v >> 3) << 4)));
    for (unsigned v = 0; v < 64; ++v) bytes.push_back(static_cast<std::uint8_t>(v));
    for (unsigned code = 0; code < 512; ++code) {
        const std::uint16_t ctrl = code == 0 ? 0x46 : 0;
        bytes.push_back(static_cast<std::uint8_t>(ctrl & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(ctrl >> 8));
    }
    bytes.resize(kBcpackTablesSize, 0);
    return bytes;
}

const Tables& tables() {
    static const Tables parsed = [] {
        BcpackError error{};
        auto value = Tables::parse(test_tables(), &error);
        CHECK(value.has_value());
        return *value;
    }();
    return parsed;
}

std::vector<std::uint8_t> fragment(std::uint8_t width, const std::vector<std::uint8_t>& symbols) {
    const std::uint32_t f = 1u << (15 - width);
    std::uint32_t x = kBcpackLow;
    std::vector<std::uint8_t> bytes;
    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
        const std::uint8_t s = *it;
        const std::uint32_t freq = s == 0 ? f - 1 : f;
        const std::uint32_t start = s == 0 ? 1u : static_cast<std::uint32_t>(s) * f;
        while (x >= freq << 16) {
            bytes.push_back(static_cast<std::uint8_t>(x));
            x >>= 8;
        }
        x = ((x / freq) << 16) + x % freq + start;
    }
    for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(x >> (8 * k)));
    std::reverse(bytes.begin(), bytes.end());
    std::vector<std::uint8_t> out{0};
    if (bytes.size() > 256) {
        out[0] |= 0x20;
        const std::size_t n = bytes.size() - 257;
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
    } else {
        out.push_back(static_cast<std::uint8_t>(bytes.size() - 1));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

constexpr std::array<std::uint8_t, 12> kLanes4{7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::array<std::uint8_t, 16> kLanes7{3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};

std::vector<std::uint8_t> stream(const std::vector<std::uint8_t>& widths, std::size_t per,
                                 const std::vector<std::vector<std::uint8_t>>& records) {
    const std::size_t stride = widths.size() * per;
    std::vector<std::deque<std::uint8_t>> queues(widths.size());
    for (std::size_t j = 0; j < widths.size(); ++j) {
        std::vector<std::uint8_t> symbols;
        for (const auto& record : records)
            for (std::size_t k = 0; k < per; ++k) symbols.push_back(record[j * per + k]);
        std::vector<std::uint8_t> bytes = fragment(widths[j], symbols);
        bytes.insert(bytes.end(), {0, 1, 0, 0});
        queues[j].assign(bytes.begin(), bytes.end());
    }
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> check(records.size() * stride, 0);
    bool asked_past_end = false;
    const BcpackFetch fetch = [&](std::size_t lane) -> std::optional<std::uint8_t> {
        if (queues[lane].empty()) {
            asked_past_end = true;
            return std::nullopt;
        }
        const std::uint8_t byte = queues[lane].front();
        queues[lane].pop_front();
        out.push_back(byte);
        return byte;
    };
    const auto error = lanes(tables(), std::span<const std::uint8_t>(widths), per, records.size(),
                             fetch, check);
    CHECK(!error.has_value());
    CHECK(!asked_past_end);
    std::vector<std::uint8_t> expected;
    for (const auto& record : records) expected.insert(expected.end(), record.begin(), record.end());
    CHECK(check == expected);
    return out;
}

std::uint8_t zigzag(int value) {
    return static_cast<std::uint8_t>(value >= 0 ? 2 * value : -2 * value - 1);
}

std::array<std::uint8_t, 8> block_bytes(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t k = 0; k < 8; ++k) bytes[k] = static_cast<std::uint8_t>(value >> (8 * k));
    return bytes;
}

struct BitWriter {
    std::array<std::uint64_t, 2> v{};
    std::uint32_t at = 0;

    void put(std::uint64_t value, std::uint32_t n) {
        if (n > 0 && at < 128) {
            const std::uint64_t mask = (std::uint64_t{1} << n) - 1;
            value &= mask;
            const unsigned shift = at & 63u;
            v[at >> 6] |= value << shift;
            if (shift != 0 && (at >> 6) == 0) v[1] |= value >> (64 - shift);
        }
        at += n;
    }

    std::array<std::uint8_t, 16> bytes() const {
        std::array<std::uint8_t, 16> out{};
        for (std::size_t k = 0; k < 8; ++k) {
            out[k] = static_cast<std::uint8_t>(v[0] >> (8 * k));
            out[8 + k] = static_cast<std::uint8_t>(v[1] >> (8 * k));
        }
        return out;
    }
};

}  // namespace

TEST(bcpack_table_file_parses_when_provided) {
    const DWORD needed = GetEnvironmentVariableW(L"G6SHIM_BCPACK_TABLES", nullptr, 0);
    if (needed == 0) {
        std::printf("note: G6SHIM_BCPACK_TABLES is not set; skipping the table file check\n");
        return;
    }
    std::wstring path(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"G6SHIM_BCPACK_TABLES", path.data(), needed);
    CHECK(written > 0 && written < needed);
    if (written == 0 || written >= needed) return;
    path.resize(written);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    CHECK(GetFileSizeEx(file, &size) != 0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart), 0);
    DWORD got = 0;
    CHECK(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) != 0);
    CHECK(got == bytes.size());
    CloseHandle(file);
    BcpackError error{};
    CHECK(Tables::parse(bytes, &error).has_value());
}

TEST(bc4_blocks_with_and_without_references) {
    const Tables& t = tables();
    std::vector<std::uint8_t> b0{0, 0, zigzag(10), zigzag(-3)};
    for (unsigned k = 0; k < 8; ++k)
        b0.push_back(static_cast<std::uint8_t>(((2 * k) & 7) | (((2 * k + 1) & 7) << 3)));
    std::vector<std::uint8_t> b1{0, 1, zigzag(1), zigzag(1)};
    for (unsigned k = 0; k < 8; ++k) b1.push_back(static_cast<std::uint8_t>(1 | (1 << 3)));
    const auto src = stream(std::vector<std::uint8_t>(kLanes4.begin(), kLanes4.end()), 1, {b0, b1});
    std::array<std::uint8_t, 16> dst{};
    CHECK(!decode(4, src, dst, t).has_value());
    const auto block = [](std::uint8_t e0, std::uint8_t e1, unsigned add) {
        const std::uint64_t code[8] = {0, 2, 3, 4, 5, 6, 7, 1};
        std::uint64_t bits = 0;
        for (unsigned p = 0; p < 16; ++p) bits |= code[(p + add) & 7] << (16 + 3 * p);
        return block_bytes(static_cast<std::uint64_t>(e0) | static_cast<std::uint64_t>(e1) << 8 |
                           bits);
    };
    const auto first = block(138, 125, 0);
    const auto second = block(139, 126, 1);
    CHECK(std::equal(first.begin(), first.end(), dst.begin()));
    CHECK(std::equal(second.begin(), second.end(), dst.begin() + 8));
}

TEST(bcpack_lanes_take_bytes_in_schedule_order_and_long_fragments) {
    const Tables& t = tables();
    std::vector<std::vector<std::uint8_t>> records;
    for (unsigned i = 0; i < 400; ++i) {
        std::vector<std::uint8_t> record{0, 0, static_cast<std::uint8_t>(i * 7),
                                         static_cast<std::uint8_t>(i * 13)};
        for (unsigned k = 0; k < 8; ++k)
            record.push_back(static_cast<std::uint8_t>((i + k) * 5 % 64));
        records.push_back(std::move(record));
    }
    const auto src = stream(std::vector<std::uint8_t>(kLanes4.begin(), kLanes4.end()), 1, records);
    std::vector<std::uint8_t> got(records.size() * 12, 0);
    std::size_t next = 0;
    const BcpackFetch fetch = [&](std::size_t) -> std::optional<std::uint8_t> {
        if (next >= src.size()) return std::nullopt;
        return src[next++];
    };
    CHECK(!lanes(t, kLanes4, 1, records.size(), fetch, got).has_value());
    std::vector<std::uint8_t> expected;
    for (const auto& record : records) expected.insert(expected.end(), record.begin(), record.end());
    CHECK(got == expected);
}

TEST(bc7_mode6_block) {
    const Tables& t = tables();
    std::vector<std::uint8_t> s(32, 0);
    s[4] = zigzag(0x55);
    s[5] = zigzag(-1);
    s[6] = zigzag(3);
    s[10] = zigzag(0x7f >> 1);
    const auto src = stream(std::vector<std::uint8_t>(kLanes7.begin(), kLanes7.end()), 2, {s});
    std::array<std::uint8_t, 16> dst{};
    CHECK(!decode(7, src, dst, t).has_value());
    BitWriter w;
    w.put(1 << 6, 7);
    const std::pair<std::uint64_t, std::uint32_t> fields[] = {
        {0x2a, 7}, {0x7f, 7}, {3, 7}, {0, 7}, {0, 7}, {0, 7}, {0x3f, 7}, {0, 7}};
    for (const auto& field : fields) w.put(field.first, field.second);
    w.put(1, 1);
    w.put(1, 1);
    CHECK(dst == w.bytes());
}

TEST(bcpack_malformed_input_is_an_error) {
    const Tables& t = tables();
    std::vector<std::vector<std::uint8_t>> records;
    for (unsigned i = 0; i < 64; ++i)
        records.push_back({0, 0, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), 1, 2, 3,
                           4, 5, 6, 7, 8});
    const auto src = stream(std::vector<std::uint8_t>(kLanes4.begin(), kLanes4.end()), 1, records);
    std::vector<std::uint8_t> dst(64 * 8, 0);
    CHECK(!decode(4, src, dst, t).has_value());
    CHECK(decode(4, std::span<const std::uint8_t>(src).first(src.size() / 2), dst, t).has_value());
    const std::vector<std::uint8_t> headers(16, 0x30);
    CHECK(decode(4, headers, std::span<std::uint8_t>(dst).first(8), t).has_value());
    CHECK(decode(4, std::span<const std::uint8_t>{}, dst, t).has_value());
    CHECK(decode(4, src, std::span<std::uint8_t>(dst).first(12), t).has_value());
    CHECK(decode(4, src, std::span<std::uint8_t>{}, t).has_value());
    std::vector<std::uint8_t> roomier(65 * 8, 0);
    CHECK(decode(4, src, roomier, t).has_value());
    CHECK(decode(2, src, dst, t).has_value());
    const std::vector<std::uint8_t> short_table(16, 0);
    CHECK(!Tables::parse(short_table, nullptr).has_value());
    std::vector<std::uint8_t> no_tiling = test_tables();
    no_tiling[0] = 1;
    BcpackError error{};
    CHECK(!Tables::parse(no_tiling, &error).has_value());
}

TEST(bcpack_noise_never_panics) {
    const Tables& t = tables();
    std::uint64_t seed = 0x2545f4914f6cdd1dull;
    for (unsigned round = 0; round < 300; ++round) {
        const std::size_t len = round * 7 % 900;
        std::vector<std::uint8_t> src(len, 0);
        for (auto& byte : src) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            byte = static_cast<std::uint8_t>(seed);
        }
        for (const std::uint8_t bc : std::array<std::uint8_t, 6>{1, 3, 4, 5, 6, 7}) {
            std::vector<std::uint8_t> dst(16 * (1 + round % 40), 0);
            (void)decode(bc, src, dst, t);
        }
    }
}

TEST(bcpack_external_chunks) {
    const DWORD needed = 2 * MAX_PATH;
    std::string tables_path(needed, '\0');
    std::string cases_path(needed, '\0');
    const DWORD tables_len =
        GetEnvironmentVariableA("G6SHIM_BCPACK_TABLES", tables_path.data(), needed);
    const DWORD cases_len = GetEnvironmentVariableA("G6SHIM_BCPACK_CASES", cases_path.data(), needed);
    if (tables_len == 0 || cases_len == 0) {
        std::printf("note: G6SHIM_BCPACK_TABLES/G6SHIM_BCPACK_CASES not set; skipping the chunk "
                    "checks\n");
        return;
    }
    std::string tables_bytes;
    {
        std::FILE* file = std::fopen(tables_path.c_str(), "rb");
        CHECK(file != nullptr);
        if (file == nullptr) return;
        std::array<char, 4096> buffer{};
        std::size_t got = 0;
        while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) != 0)
            tables_bytes.append(buffer.data(), got);
        std::fclose(file);
    }
    BcpackError error{};
    const auto parsed = Tables::parse(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(tables_bytes.data()),
                                      tables_bytes.size()),
        &error);
    CHECK(parsed.has_value());
    if (!parsed) return;
    std::size_t checked = 0;
    WIN32_FIND_DATAA entry{};
    const std::string pattern = cases_path + "\\*";
    const HANDLE find = FindFirstFileA(pattern.c_str(), &entry);
    CHECK(find != INVALID_HANDLE_VALUE);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = entry.cFileName;
        const auto dot = name.rfind('.');
        if (dot == std::string::npos || name.compare(dot, 3, ".bc") != 0) continue;
        const std::string number = name.substr(dot + 3);
        const int bc = std::atoi(number.c_str());
        std::vector<std::uint8_t> packed;
        std::vector<std::uint8_t> want;
        const auto read_all = [](const std::string& path, std::vector<std::uint8_t>& into) {
            std::FILE* file = std::fopen(path.c_str(), "rb");
            if (file == nullptr) return false;
            std::array<std::uint8_t, 4096> buffer{};
            std::size_t got = 0;
            while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) != 0)
                into.insert(into.end(), buffer.begin(), buffer.begin() + got);
            std::fclose(file);
            return true;
        };
        CHECK(read_all(cases_path + "\\" + name, packed));
        std::string out_name = name.substr(0, dot) + ".out";
        CHECK(read_all(cases_path + "\\" + out_name, want));
        std::vector<std::uint8_t> got(want.size(), 0);
        CHECK(!decode(static_cast<std::uint8_t>(bc), packed, got, *parsed).has_value());
        CHECK(got == want);
        ++checked;
    } while (FindNextFileA(find, &entry) != 0);
    FindClose(find);
    CHECK(checked > 0);
}
