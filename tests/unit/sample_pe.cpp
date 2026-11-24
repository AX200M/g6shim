// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_pe.hpp"

#include <fstream>
#include <random>
#include <string>

namespace g6shim::test {

namespace {

struct SectionData {
    static constexpr std::uint32_t kVa = 0x1000;
    std::vector<std::uint8_t> data;

    std::uint32_t add(const void* p, std::size_t n, std::size_t align = 1) {
        while (data.size() % align) data.push_back(0);
        const auto rva = static_cast<std::uint32_t>(kVa + data.size());
        const auto* b = static_cast<const std::uint8_t*>(p);
        data.insert(data.end(), b, b + n);
        return rva;
    }
    std::uint32_t reserve(std::size_t n, std::size_t align) {
        std::vector<std::uint8_t> zeros(n);
        return add(zeros.data(), n, align);
    }
    std::uint32_t str(const std::string& s) { return add(s.c_str(), s.size() + 1); }
    std::uint32_t hint_name(std::uint16_t hint, const std::string& name) {
        const std::uint32_t rva = add(&hint, 2, 2);
        str(name);
        return rva;
    }
    std::uint32_t thunks(std::vector<std::uint64_t> v) {
        v.push_back(0);
        return add(v.data(), v.size() * 8, 8);
    }
    template <class T>
    void patch(std::uint32_t rva, T v) {
        std::memcpy(&data[rva - kVa], &v, sizeof v);
    }
};

}  // namespace

std::vector<std::uint8_t> build_sample_pe() {
    SectionData s;
    const std::uint32_t n_fake = s.str("fakeconsole.dll");
    const std::uint32_t n_k32 = s.str("KERNEL32.dll");
    const std::uint32_t n_delay = s.str("fakedelay.dll");
    const std::uint32_t foo = s.hint_name(1, "Foo");
    const std::uint32_t exit_process = s.hint_name(0, "ExitProcess");
    const std::uint32_t delay_fn = s.hint_name(0, "DelayFn");

    const std::uint64_t by_ordinal = 1ull << 63;
    const std::uint32_t int_fake = s.thunks({foo, by_ordinal | 7});
    const std::uint32_t iat_fake = s.thunks({foo, by_ordinal | 7});
    const std::uint32_t int_k32 = s.thunks({exit_process});
    const std::uint32_t iat_k32 = s.thunks({exit_process});
    const std::uint32_t delay_int = s.thunks({delay_fn});
    const std::uint32_t delay_iat = s.thunks({0});
    const std::uint32_t delay_handle = s.reserve(8, 8);

    const std::uint32_t import_desc[15] = {int_fake, 0, 0, n_fake, iat_fake, int_k32, 0, 0, n_k32, iat_k32, 0, 0, 0, 0, 0};
    const std::uint32_t import_rva = s.add(import_desc, sizeof import_desc, 4);
    const std::uint32_t delay_desc[16] = {1, n_delay, delay_handle, delay_iat, delay_int, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint32_t delay_rva = s.add(delay_desc, sizeof delay_desc, 4);

    const std::uint32_t exp = s.reserve(40, 4);
    const std::uint32_t functions = s.reserve(12, 4);
    const std::uint32_t names = s.reserve(8, 4);
    const std::uint32_t ordinals = s.reserve(4, 2);
    const std::uint32_t n_dll = s.str("test.dll");
    const std::uint32_t n_alpha = s.str("Alpha");
    const std::uint32_t n_gamma = s.str("Gamma");
    const std::uint32_t forwarder = s.str("other.Beta");
    const auto exp_end = static_cast<std::uint32_t>(SectionData::kVa + s.data.size());
    s.patch<std::uint32_t>(exp + 12, n_dll);
    s.patch<std::uint32_t>(exp + 16, 1);
    s.patch<std::uint32_t>(exp + 20, 3);
    s.patch<std::uint32_t>(exp + 24, 2);
    s.patch<std::uint32_t>(exp + 28, functions);
    s.patch<std::uint32_t>(exp + 32, names);
    s.patch<std::uint32_t>(exp + 36, ordinals);
    s.patch<std::uint32_t>(functions, 0x1f00);
    s.patch<std::uint32_t>(functions + 4, 0x1f10);
    s.patch<std::uint32_t>(functions + 8, forwarder);
    s.patch<std::uint32_t>(names, n_alpha);
    s.patch<std::uint32_t>(names + 4, n_gamma);
    s.patch<std::uint16_t>(ordinals, 0);
    s.patch<std::uint16_t>(ordinals + 2, 2);
    s.data.resize(0x1000);

    std::vector<std::uint8_t> b(kSampleHeaders);
    put<std::uint16_t>(b, 0, 0x5a4d);
    put<std::uint32_t>(b, 0x3c, 0x40);
    put<std::uint32_t>(b, 0x40, 0x4550);
    const std::size_t coff = kSampleCoff;
    put<std::uint16_t>(b, coff, 0x8664);
    put<std::uint16_t>(b, coff + 2, 1);
    put<std::uint16_t>(b, coff + 16, 240);
    put<std::uint16_t>(b, coff + 18, 0x2022);
    const std::size_t opt = kSampleOptional;
    put<std::uint16_t>(b, opt, 0x20b);
    put<std::uint32_t>(b, opt + 16, 0x1f00);
    put<std::uint64_t>(b, opt + 24, 0x180000000ull);
    put<std::uint32_t>(b, opt + 32, 0x1000);
    put<std::uint32_t>(b, opt + 36, 0x200);
    put<std::uint32_t>(b, opt + 56, 0x2000);
    put<std::uint32_t>(b, opt + 60, 0x200);
    put<std::uint16_t>(b, opt + 68, 14);
    put<std::uint32_t>(b, opt + 108, 16);
    auto dir = [&](int i, std::uint32_t rva, std::uint32_t size) {
        put<std::uint32_t>(b, kSampleDirectories + i * 8, rva);
        put<std::uint32_t>(b, kSampleDirectories + i * 8 + 4, size);
    };
    dir(0, exp, exp_end - exp);
    dir(1, import_rva, sizeof import_desc);
    dir(13, delay_rva, sizeof delay_desc);
    const std::size_t sec = kSampleSections;
    std::memcpy(&b[sec], ".rdata", 6);
    put<std::uint32_t>(b, sec + 8, 0x1000);
    put<std::uint32_t>(b, sec + 12, 0x1000);
    put<std::uint32_t>(b, sec + 16, 0x1000);
    put<std::uint32_t>(b, sec + 20, 0x200);
    put<std::uint32_t>(b, sec + 36, 0x40000040);
    b.insert(b.end(), s.data.begin(), s.data.end());
    return b;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

TempDir::TempDir() {
    path = std::filesystem::temp_directory_path() / ("g6shim_tests_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace g6shim::test
