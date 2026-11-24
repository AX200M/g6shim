// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace g6shim::chad {

inline constexpr std::uint32_t kConvolution = 1;
inline constexpr std::uint32_t kStereoOptions = 2 | 8;
inline constexpr std::uint32_t kNotForConvolution = 4;
inline constexpr std::uint32_t kMinBaseLength = 0x10000;
inline constexpr std::uint32_t kMaxBaseLength = 0x1F400000;
inline constexpr std::uint32_t kMaxIrSeconds = 0x80;
inline constexpr std::uint32_t kMaxStreams = 0x100;
inline constexpr std::uint32_t kMaxChannels = 2;

#pragma pack(push, 1)
struct StreamDesc {
    std::uint32_t kind;
    std::uint32_t options;
    std::uint32_t frames;
    std::uint32_t channels;
    std::uint32_t ir_floats;
    const float* ir;
};
#pragma pack(pop)

struct StreamStatus {
    std::atomic<std::uint32_t> submitted{0};
    std::atomic<std::uint32_t> completed{0};
    std::uint32_t reserved{0};
    std::atomic<HRESULT> result{S_OK};
};

struct Command {
    float gains[2];
    const float* input;
    float* output;
    std::uint64_t reserved;
};

struct Dsp {
    std::uint32_t max_streams;
    std::mutex mutex;
    std::uint32_t active{0};
};

struct Stream {
    Dsp* dsp;
    std::uint32_t frames;
    std::uint32_t channels;
    StreamStatus status;
};

bool stream_shape_valid(const StreamDesc& desc);

HRESULT connect(void* base, std::uint32_t base_length, std::uint32_t ir_seconds, std::uint32_t max_streams,
                std::uint32_t max_channels, Dsp** out);
HRESULT disconnect(Dsp* dsp);
HRESULT activate(Dsp* dsp, const StreamDesc* desc, const StreamStatus** status, Stream** stream);
HRESULT deactivate(Stream* stream);
HRESULT submit_command(Stream* stream, const Command* command, std::uint32_t* ticket);

}  // namespace g6shim::chad
