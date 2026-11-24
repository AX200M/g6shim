// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "apu.hpp"

#include <windows.h>

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <malloc.h>

namespace g6shim::xapu {

namespace {

constexpr std::size_t kMemoryAlignment = 16;
constexpr std::uint32_t kBytesPerSample = 4;
constexpr float kUnityPitch = 1.0f;

constexpr std::size_t kMaxFrameBytes = 1275;
constexpr std::uint32_t kMaxPacketFrames = 48;
constexpr std::uint8_t kTwoByteFrameLength = 252;
constexpr std::uint8_t kPaddingContinues = 255;
constexpr std::size_t kPaddingContinueBytes = 254;

constexpr std::uint32_t kSilkFrameSamples[] = {480, 960, 1920, 2880};
constexpr std::uint32_t kHybridFrameSamples[] = {480, 960};
constexpr std::uint32_t kCeltFrameSamples[] = {120, 240, 480, 960};

std::uint32_t frame_samples(std::uint8_t toc) {
    const std::uint32_t config = toc >> 3;
    if (config < 12) return kSilkFrameSamples[config & 3u];
    if (config < 16) return kHybridFrameSamples[config & 1u];
    return kCeltFrameSamples[config & 3u];
}

std::optional<std::uint32_t> packet_frame_count(const std::uint8_t* data, std::size_t length) {
    if (length == 0) return std::nullopt;
    const std::uint32_t code = data[0] & 3u;
    if (code == 0) {
        if (length - 1 > kMaxFrameBytes) return std::nullopt;
        return 1u;
    }
    if (length < 2) return std::nullopt;
    if (code == 1) {
        if ((length - 1) % 2 != 0 || (length - 1) / 2 > kMaxFrameBytes) return std::nullopt;
        return 2u;
    }
    if (code == 2) {
        if (length < 3) return std::nullopt;
        const std::size_t first = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
        if (first > kMaxFrameBytes || first > length - 3) return std::nullopt;
        if (length - 3 - first > kMaxFrameBytes) return std::nullopt;
        return 2u;
    }
    const std::uint32_t count = data[1] & 0x3Fu;
    if (count == 0 || count > kMaxPacketFrames) return std::nullopt;
    const bool variable = (data[1] & 0x80u) != 0;
    const bool padded = (data[1] & 0x40u) != 0;
    std::size_t offset = 2;
    std::size_t padding = 0;
    if (padded) {
        for (;;) {
            if (offset >= length) return std::nullopt;
            const std::uint8_t value = data[offset++];
            padding += value == kPaddingContinues ? kPaddingContinueBytes : value;
            if (value != kPaddingContinues) break;
        }
    }
    if (padding > length - offset) return std::nullopt;
    std::array<std::size_t, kMaxPacketFrames> sizes{};
    if (variable) {
        for (std::uint32_t i = 0; i + 1 < count; ++i) {
            if (offset >= length) return std::nullopt;
            std::size_t size = data[offset++];
            if (size >= kTwoByteFrameLength) {
                if (offset >= length) return std::nullopt;
                size += 4 * static_cast<std::size_t>(data[offset++]);
            }
            if (size > kMaxFrameBytes) return std::nullopt;
            sizes[i] = size;
        }
    }
    const std::size_t remaining = length - offset - padding;
    if (!variable && remaining % count != 0) return std::nullopt;
    std::size_t used = 0;
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
        const std::size_t size = variable ? sizes[i] : remaining / count;
        if (size > remaining - used) return std::nullopt;
        used += size;
    }
    if (remaining - used > kMaxFrameBytes) return std::nullopt;
    return count;
}

std::optional<std::uint32_t> packet_frames(const std::uint8_t* data, std::size_t length) {
    const std::optional<std::uint32_t> count = packet_frame_count(data, length);
    if (!count) return std::nullopt;
    const std::uint64_t frames = static_cast<std::uint64_t>(*count) * frame_samples(data[0]);
    if (frames > kMaxFrames) return std::nullopt;
    return static_cast<std::uint32_t>(frames);
}

struct Decoder {
    std::uint32_t channels;
};

struct State {
    std::vector<std::optional<Decoder>> streams;
    std::vector<std::uint32_t> queued;
    std::deque<Result> results;
    std::uint32_t commands = 0;
    std::uint32_t dequeued = 0;
};

}

struct Connection {
    std::uint8_t* memory;
    void* event;
    std::uint32_t options;
    std::uint32_t per_stream;
    std::mutex mutex;
    State state;
};

namespace {

void process(const Decoder& decoder, const ProcessCommand& command, Result& result) {
    result.input = command.input;
    result.input_length = command.input_length;
    result.output = command.output;
    if (command.input == nullptr || command.output == nullptr || command.input_length == 0) {
        result.error = kInvalidArg;
        return;
    }
    const std::optional<std::uint32_t> decoded = packet_frames(command.input, command.input_length);
    if (!decoded) {
        result.error = kBadPacket;
        return;
    }
    const std::size_t first = std::min<std::size_t>(command.first_frame, *decoded);
    const std::size_t frames = std::min({static_cast<std::size_t>(*decoded) - first,
                                         static_cast<std::size_t>(command.frame_count),
                                         static_cast<std::size_t>(command.output_length) /
                                             (kBytesPerSample * decoder.channels)});
    const std::size_t bytes = frames * decoder.channels * kBytesPerSample;
    std::memset(command.output, 0, bytes);
    result.frames = static_cast<std::uint32_t>(frames);
    result.output_length = static_cast<std::uint32_t>(bytes);
}

}

HResult connect(const ConnectInput* input, ConnectOutput* output, Connection** handle) {
    if (input == nullptr || output == nullptr || handle == nullptr) return kInvalidArg;
    if (input->streams == 0 || input->streams > kMaxStreams || input->commands_per_stream == 0 ||
        input->commands_per_stream > kMaxCommandsPerStream || input->memory == 0)
        return kInvalidArg;
    if (input->process != kDecodeOpus) {
        G6SHIM_WARN("xapu: processing kind %u is not implemented", input->process);
        return kNotImpl;
    }
    if (input->frames_per_output != 0) return kInvalidArg;
    auto* memory = static_cast<std::uint8_t*>(_aligned_malloc(input->memory, kMemoryAlignment));
    if (memory == nullptr) return kOutOfMemory;
    std::memset(memory, 0, input->memory);
    void* event = nullptr;
    if ((input->options & kResultEvent) != 0) event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::unique_ptr<Connection> connection(new Connection{});
    connection->memory = memory;
    connection->event = event;
    connection->options = input->options;
    connection->per_stream = input->commands_per_stream;
    connection->state.streams.resize(input->streams);
    connection->state.queued.assign(input->streams, 0);
    G6SHIM_INFO("xapu: audio decoder connected, %u streams, %u bytes of memory, options 0x%X", input->streams,
                input->memory, input->options);
    output->memory = memory;
    output->result_event = event;
    *handle = connection.release();
    return kOk;
}

HResult disconnect(Connection* handle) {
    if (handle == nullptr) return kInvalidArg;
    if (handle->event != nullptr) CloseHandle(static_cast<HANDLE>(handle->event));
    _aligned_free(handle->memory);
    delete handle;
    return kOk;
}

HResult enqueue_command(Connection* handle, const void* command, std::uint32_t* total) {
    if (handle == nullptr || command == nullptr) return kInvalidArg;
    CommandId id{};
    std::memcpy(&id, command, sizeof id);
    std::lock_guard<std::mutex> lock(handle->mutex);
    const std::size_t stream = id.stream;
    if (stream >= handle->state.streams.size()) return kInvalidStream;
    if (handle->state.queued[stream] >= handle->per_stream) return kQueueFull;
    Result result{};
    result.id = id;
    result.error = kOk;
    result.pitch = kUnityPitch;
    std::optional<Decoder>& decoder = handle->state.streams[stream];
    switch (id.kind) {
        case kActivate: {
            ActivateCommand activate{};
            std::memcpy(&activate, command, sizeof activate);
            G6SHIM_TRACE("xapu: stream %u activated with %u channels", id.stream, activate.channels);
            if (decoder) {
                result.error = kAlreadyActive;
            } else if (activate.channels == 0 || activate.channels > kMaxChannels) {
                result.error = kInvalidArg;
            } else {
                decoder = Decoder{activate.channels};
            }
            break;
        }
        case kDeactivate:
            decoder.reset();
            break;
        case kReset:
            break;
        case kNotify:
            break;
        case kProcess: {
            ProcessCommand process_command{};
            std::memcpy(&process_command, command, sizeof process_command);
            if (decoder)
                process(*decoder, process_command, result);
            else
                result.error = kInvalidStream;
            break;
        }
        default:
            return kInvalidCommand;
    }
    ++handle->state.queued[stream];
    ++handle->state.commands;
    handle->state.results.push_back(result);
    if (total != nullptr) *total = handle->state.commands;
    if ((id.options & kSignal) != 0 && handle->event != nullptr) SetEvent(static_cast<HANDLE>(handle->event));
    return kOk;
}

HResult dequeue_result(Connection* handle, Result* result) {
    if (handle == nullptr || result == nullptr) return kInvalidArg;
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->state.results.empty()) return kPendingResults;
    const Result next = handle->state.results.front();
    handle->state.results.pop_front();
    --handle->state.queued[next.id.stream];
    ++handle->state.dequeued;
    std::memcpy(result, &next, sizeof next);
    return kOk;
}

std::uint32_t result_count(Connection* handle, std::uint32_t* total) {
    if (handle == nullptr) return 0;
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (total != nullptr) *total = handle->state.dequeued;
    return static_cast<std::uint32_t>(handle->state.results.size());
}

}  // namespace g6shim::xapu

extern "C" HRESULT g6shim_XApuConnect(const g6shim::xapu::ConnectInput* input, g6shim::xapu::ConnectOutput* output,
                                      g6shim::xapu::Connection** handle) {
    return g6shim::xapu::connect(input, output, handle);
}

extern "C" HRESULT g6shim_XApuDisconnect(g6shim::xapu::Connection* handle) { return g6shim::xapu::disconnect(handle); }

extern "C" HRESULT g6shim_XApuEnqueueCommand(g6shim::xapu::Connection* handle, const void* command,
                                             std::uint32_t* total) {
    return g6shim::xapu::enqueue_command(handle, command, total);
}

extern "C" HRESULT g6shim_XApuDequeueResult(g6shim::xapu::Connection* handle, g6shim::xapu::Result* result) {
    return g6shim::xapu::dequeue_result(handle, result);
}

extern "C" std::uint32_t g6shim_XApuGetResultCount(g6shim::xapu::Connection* handle, std::uint32_t* total) {
    return g6shim::xapu::result_count(handle, total);
}
