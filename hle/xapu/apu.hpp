// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g6shim::xapu {

using HResult = std::int32_t;

inline constexpr HResult kOk = 0;
inline constexpr HResult kInvalidArg = static_cast<HResult>(0x80070057u);
inline constexpr HResult kNotImpl = static_cast<HResult>(0x80004001u);
inline constexpr HResult kOutOfMemory = static_cast<HResult>(0x8007000Eu);
inline constexpr HResult kQueueFull = static_cast<HResult>(0x8AC81000u);
inline constexpr HResult kPendingResults = static_cast<HResult>(0x8AC81001u);
inline constexpr HResult kInvalidCommand = static_cast<HResult>(0x8AC81002u);
inline constexpr HResult kInvalidStream = static_cast<HResult>(0x8AC81003u);
inline constexpr HResult kAlreadyActive = static_cast<HResult>(0x8AC81005u);
inline constexpr HResult kBadPacket = static_cast<HResult>(0x8AC81008u);

inline constexpr std::uint32_t kDecodeOpus = 1;

inline constexpr std::uint32_t kActivate = 1;
inline constexpr std::uint32_t kDeactivate = 2;
inline constexpr std::uint32_t kProcess = 3;
inline constexpr std::uint32_t kNotify = 4;
inline constexpr std::uint32_t kReset = 5;

inline constexpr std::uint32_t kSignal = 1;
inline constexpr std::uint32_t kDeinterleavedOutput = 4;
inline constexpr std::uint32_t kResultEvent = 8;

inline constexpr std::uint32_t kMaxStreams = 500;
inline constexpr std::uint32_t kMaxCommandsPerStream = 16;
inline constexpr std::uint32_t kMaxChannels = 2;
inline constexpr std::uint32_t kMaxFrames = 5760;
inline constexpr std::uint32_t kSampleRate = 48000;

#pragma pack(push, 1)
struct CommandId {
    std::uint32_t kind;
    std::uint32_t stream;
    std::uint32_t sequence;
    std::uint32_t options;
    std::uint64_t context;
};

struct ActivateCommand {
    CommandId id;
    std::uint32_t channels;
    std::uint32_t init_length;
    const void* init;
    float start_pitch;
    std::uint32_t processing_length;
    void* processing;
};

struct ProcessCommand {
    CommandId id;
    float pitch_ramp;
    float target_pitch;
    std::uint32_t first_frame;
    std::uint32_t frame_count;
    std::uint32_t input_length;
    const std::uint8_t* input;
    std::uint32_t output_length;
    std::uint8_t* output;
};

struct Result {
    CommandId id;
    std::uint32_t state;
    std::uint64_t state_data;
    std::uint64_t usec;
    HResult error;
    std::uint32_t frames;
    std::uint32_t output_length;
    std::uint8_t* output;
    float pitch;
    std::uint32_t input_length;
    const std::uint8_t* input;
};
#pragma pack(pop)

static_assert(sizeof(CommandId) == 0x18);
static_assert(sizeof(ActivateCommand) == 0x38);
static_assert(sizeof(ProcessCommand) == 0x40);
static_assert(sizeof(Result) == 0x50);

struct ConnectInput {
    std::uint32_t process;
    std::uint32_t streams;
    std::uint32_t commands_per_stream;
    std::uint32_t memory;
    std::uint32_t frames_per_output;
    std::uint32_t options;
};

struct ConnectOutput {
    void* memory;
    void* result_event;
};

struct Connection;

HResult connect(const ConnectInput* input, ConnectOutput* output, Connection** handle);
HResult disconnect(Connection* handle);
HResult enqueue_command(Connection* handle, const void* command, std::uint32_t* total);
HResult dequeue_result(Connection* handle, Result* result);
std::uint32_t result_count(Connection* handle, std::uint32_t* total);

}  // namespace g6shim::xapu
