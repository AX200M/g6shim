// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "speech.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

namespace sp = g6shim::xgameruntime::speech;
using g6shim::xgameruntime::HResult;
using g6shim::xgameruntime::Object;

using Create = HResult (*)(Object*, const void**);
using Stream = HResult (*)(Object*, const void*, const char*, const void**);
using Size = HResult (*)(Object*, const void*, std::size_t*);
using Data = HResult (*)(Object*, const void*, std::size_t, std::uint8_t*, std::size_t*);

template <class Function>
Function at(std::size_t offset) {
    return g6shim::xgameruntime::method<Function>(sp::kObject, offset);
}

Object* self() { return const_cast<Object*>(&sp::kObject); }

}  // namespace

TEST(xgameruntime_speech_streams_are_empty_wave_files) {
    void* served = nullptr;
    CHECK(g6shim::xgameruntime::query_api(&sp::kProvider, &sp::kIid, &served) == g6shim::xgameruntime::kOk);
    CHECK(served == const_cast<Object*>(&sp::kObject));

    const void* synthesizer = nullptr;
    CHECK(at<Create>(0x48)(self(), &synthesizer) == g6shim::xgameruntime::kOk);
    CHECK(synthesizer != nullptr);

    const void* stream = nullptr;
    CHECK(at<Stream>(0x68)(self(), synthesizer, "Hello", &stream) == g6shim::xgameruntime::kOk);
    CHECK(stream != nullptr);

    std::size_t size = 0;
    CHECK(at<Size>(0x78)(self(), stream, &size) == g6shim::xgameruntime::kOk);
    CHECK(size == sp::kSilence.size());

    std::vector<std::uint8_t> wave(size, 0);
    std::size_t used = 0;
    CHECK(at<Data>(0x80)(self(), stream, size, wave.data(), &used) == g6shim::xgameruntime::kOk);
    CHECK(std::memcmp(wave.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(wave.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(std::memcmp(wave.data() + 36, "data", 4) == 0);
    CHECK(wave[24] == 0x80 && wave[25] == 0x3e);
    CHECK(used == 44);
    CHECK(at<Data>(0x80)(self(), stream, 43, wave.data(), &used) ==
          g6shim::xgameruntime::kNotSufficientBuffer);
}
