// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "apu.hpp"

#include <windows.h>

#include <g6shim/log.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <opus.h>

#include <malloc.h>

namespace g6shim::xapu {

namespace {

constexpr std::size_t kMemoryAlignment = 16;
constexpr std::uint32_t kBytesPerSample = 4;
constexpr float kUnityPitch = 1.0f;
constexpr std::size_t kScratchSamples = static_cast<std::size_t>(kMaxFrames) * kMaxChannels;

struct OpusDecoderDeleter {
    void operator()(OpusDecoder* decoder) const { opus_decoder_destroy(decoder); }
};

struct Decoder {
    std::unique_ptr<OpusDecoder, OpusDecoderDeleter> opus;
    std::uint32_t channels;
};

std::optional<Decoder> make_decoder(std::uint32_t channels) {
    int error = OPUS_OK;
    std::unique_ptr<OpusDecoder, OpusDecoderDeleter> opus(
        opus_decoder_create(static_cast<opus_int32>(kSampleRate), static_cast<int>(channels), &error));
    if (error != OPUS_OK || opus == nullptr) return std::nullopt;
    return Decoder{std::move(opus), channels};
}

struct State {
    std::vector<std::optional<Decoder>> streams;
    std::vector<std::uint32_t> queued;
    std::deque<Result> results;
    std::vector<float> scratch;
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

void write_interleaved(const float* source, std::size_t frames, std::uint32_t channels, std::uint8_t* output) {
    std::memcpy(output, source, frames * channels * kBytesPerSample);
}

void write_deinterleaved(const float* source, std::size_t frames, std::uint32_t channels, std::uint8_t* output) {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        std::uint8_t* plane = output + static_cast<std::size_t>(channel) * frames * kBytesPerSample;
        for (std::size_t frame = 0; frame < frames; ++frame)
            std::memcpy(plane + frame * kBytesPerSample, source + frame * channels + channel, kBytesPerSample);
    }
}

void process(Decoder& decoder, bool deinterleaved, std::vector<float>& scratch, const ProcessCommand& command,
             Result& result) {
    result.input = command.input;
    result.input_length = command.input_length;
    result.output = command.output;
    if (command.input == nullptr || command.output == nullptr || command.input_length == 0) {
        result.error = kInvalidArg;
        return;
    }
    if (command.input_length > static_cast<std::uint32_t>(std::numeric_limits<opus_int32>::max())) {
        result.error = kBadPacket;
        return;
    }
    const int decoded =
        opus_decode_float(decoder.opus.get(), command.input, static_cast<opus_int32>(command.input_length),
                          scratch.data(), static_cast<int>(kMaxFrames), 0);
    if (decoded < 0) {
        result.error = kBadPacket;
        return;
    }
    const std::size_t first = std::min<std::size_t>(command.first_frame, static_cast<std::size_t>(decoded));
    const std::size_t frames = std::min({static_cast<std::size_t>(decoded) - first,
                                         static_cast<std::size_t>(command.frame_count),
                                         static_cast<std::size_t>(command.output_length) /
                                             (kBytesPerSample * decoder.channels)});
    const float* source = scratch.data() + first * decoder.channels;
    if (deinterleaved)
        write_deinterleaved(source, frames, decoder.channels, command.output);
    else
        write_interleaved(source, frames, decoder.channels, command.output);
    result.frames = static_cast<std::uint32_t>(frames);
    result.output_length = static_cast<std::uint32_t>(frames * decoder.channels * kBytesPerSample);
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
    connection->state.scratch.assign(kScratchSamples, 0.0f);
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
                decoder = make_decoder(activate.channels);
                if (!decoder) result.error = kOutOfMemory;
            }
            break;
        }
        case kDeactivate:
            decoder.reset();
            break;
        case kReset:
            if (decoder) opus_decoder_ctl(decoder->opus.get(), OPUS_RESET_STATE);
            break;
        case kNotify:
            break;
        case kProcess: {
            ProcessCommand process_command{};
            std::memcpy(&process_command, command, sizeof process_command);
            if (decoder)
                process(*decoder, (handle->options & kDeinterleavedOutput) != 0, handle->state.scratch,
                        process_command, result);
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
