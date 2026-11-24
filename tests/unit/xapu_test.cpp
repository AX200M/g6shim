// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "apu.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

using namespace g6shim::xapu;

namespace {

constexpr std::uint8_t kCeltFullbandStereo20ms = 0xFC;
constexpr std::uint8_t kCeltNarrowbandMono2p5ms = 0x80;

CommandId make_id(std::uint32_t kind, std::uint32_t stream, std::uint32_t options = 0) {
    return CommandId{kind, stream, 7, options, 0x1234};
}

ActivateCommand make_activate(std::uint32_t stream, std::uint32_t channels, std::uint32_t options = 0) {
    ActivateCommand command{};
    command.id = make_id(kActivate, stream, options);
    command.channels = channels;
    command.start_pitch = 1.0f;
    return command;
}

ProcessCommand make_process(std::uint32_t stream, const std::uint8_t* input, std::uint32_t input_length,
                            std::uint8_t* output, std::uint32_t output_length, std::uint32_t frame_count,
                            std::uint32_t first_frame = 0) {
    ProcessCommand command{};
    command.id = make_id(kProcess, stream);
    command.target_pitch = 1.0f;
    command.first_frame = first_frame;
    command.frame_count = frame_count;
    command.input_length = input_length;
    command.input = input;
    command.output_length = output_length;
    command.output = output;
    return command;
}

template <typename T, std::size_t N>
constexpr std::uint32_t bytes_of(const T (&)[N]) {
    return static_cast<std::uint32_t>(N);
}

bool silent(const std::uint8_t* bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

Result drain(Connection* handle) {
    Result result{};
    CHECK(dequeue_result(handle, &result) == kOk);
    return result;
}

}

TEST(xapu_describes_what_titles_pass) {
    CHECK(sizeof(CommandId) == 0x18);
    CHECK(sizeof(ActivateCommand) == 0x38);
    CHECK(sizeof(ProcessCommand) == 0x40);
    CHECK(sizeof(Result) == 0x50);
    CHECK(sizeof(ConnectInput) == 0x18);
    CHECK(sizeof(ConnectOutput) == 0x10);
}

TEST(xapu_validates_a_connection) {
    ConnectInput input{kDecodeOpus, 1, 1, 0x4000, 0, 0};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(nullptr, &output, &handle) == kInvalidArg);
    CHECK(connect(&input, nullptr, &handle) == kInvalidArg);
    CHECK(connect(&input, &output, nullptr) == kInvalidArg);

    ConnectInput bad = input;
    bad.streams = 0;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad.streams = kMaxStreams + 1;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad = input;
    bad.commands_per_stream = 0;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad.commands_per_stream = kMaxCommandsPerStream + 1;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad = input;
    bad.memory = 0;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad = input;
    bad.frames_per_output = 1;
    CHECK(connect(&bad, &output, &handle) == kInvalidArg);
    bad = input;
    bad.process = kDecodeOpus + 1;
    CHECK(connect(&bad, &output, &handle) == kNotImpl);
    bad.process = 0;
    CHECK(connect(&bad, &output, &handle) == kNotImpl);

    CHECK(connect(&input, &output, &handle) == kOk);
    CHECK(handle != nullptr);
    CHECK(output.memory != nullptr);
    CHECK(output.result_event == nullptr);
    CHECK(disconnect(handle) == kOk);

    input.options = kResultEvent;
    handle = nullptr;
    output = ConnectOutput{};
    CHECK(connect(&input, &output, &handle) == kOk);
    CHECK(output.result_event != nullptr);
    CHECK(disconnect(handle) == kOk);
    CHECK(disconnect(nullptr) == kInvalidArg);
}

TEST(xapu_serves_streams_as_the_title_drives_them) {
    CHECK(sizeof(Result) == 0x50);
    const ConnectInput input{kDecodeOpus, 256, 2, 0x104000, 0, 1};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);
    auto* base = static_cast<std::uint8_t*>(output.memory);
    CHECK(base != nullptr);

    ActivateCommand activate = make_activate(3, 2);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);

    const std::uint8_t packet[] = {kCeltFullbandStereo20ms, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0x10, 0x30};
    std::memcpy(base, packet, bytes_of(packet));
    ProcessCommand process = make_process(3, base, bytes_of(packet), base + 0x14000, 0xF00 * 2, 960);
    std::uint32_t total = 0;
    CHECK(enqueue_command(handle, &process, &total) == kOk);
    CHECK(total == 2);
    CHECK(enqueue_command(handle, &process, &total) == kQueueFull);
    CHECK(result_count(handle, nullptr) == 2);

    const Result first = drain(handle);
    CHECK(first.id.kind == kActivate);
    CHECK(first.error == kOk);
    CHECK(first.id.sequence == 7);
    CHECK(first.id.context == 0x1234);

    const Result decoded = drain(handle);
    CHECK(decoded.id.kind == kProcess);
    CHECK(decoded.error == kOk);
    CHECK(decoded.frames == 960);
    CHECK(decoded.output_length == 960 * 8);
    CHECK(decoded.pitch == 1.0f);
    CHECK(decoded.input == base);
    CHECK(decoded.input_length == bytes_of(packet));
    CHECK(decoded.output == base + 0x14000);
    CHECK(silent(base + 0x14000, decoded.output_length));

    Result pending{};
    CHECK(dequeue_result(handle, &pending) == kPendingResults);
    std::uint32_t dequeued = 0;
    CHECK(result_count(handle, &dequeued) == 0);
    CHECK(dequeued == 2);

    ProcessCommand stray = process;
    stray.id.stream = 4;
    CHECK(enqueue_command(handle, &stray, &total) == kOk);
    CHECK(total == 3);
    CHECK(drain(handle).error == kInvalidStream);
    CHECK(disconnect(handle) == kOk);
}

TEST(xapu_queues_results_per_stream) {
    const ConnectInput input{kDecodeOpus, 2, 1, 0x4000, 0, 0};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);
    CHECK(result_count(nullptr, nullptr) == 0);

    CommandId notify = make_id(kNotify, 0);
    std::uint32_t total = 0;
    CHECK(enqueue_command(handle, &notify, &total) == kOk);
    CHECK(total == 1);
    CHECK(enqueue_command(handle, &notify, &total) == kQueueFull);
    notify.stream = 1;
    CHECK(enqueue_command(handle, &notify, &total) == kOk);
    CHECK(total == 2);
    notify.stream = 2;
    const std::uint32_t untouched = total;
    CHECK(enqueue_command(handle, &notify, &total) == kInvalidStream);
    CHECK(total == untouched);

    std::uint32_t dequeued = 0;
    CHECK(result_count(handle, &dequeued) == 2);
    CHECK(dequeued == 0);
    CHECK(drain(handle).id.stream == 0);
    CHECK(drain(handle).id.stream == 1);
    CHECK(result_count(handle, &dequeued) == 0);
    CHECK(dequeued == 2);

    notify.stream = 0;
    notify.kind = 9;
    CHECK(enqueue_command(handle, &notify, nullptr) == kInvalidCommand);
    CHECK(result_count(handle, nullptr) == 0);
    notify.kind = kNotify;
    CHECK(enqueue_command(handle, &notify, nullptr) == kOk);
    CHECK(result_count(handle, nullptr) == 1);
    CHECK(drain(handle).error == kOk);
    CHECK(disconnect(handle) == kOk);
}

TEST(xapu_reports_command_errors_in_results) {
    const ConnectInput input{kDecodeOpus, 4, 16, 0x4000, 0, 0};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);

    std::vector<std::uint8_t> pcm(0x10000);
    const std::uint8_t packet[] = {kCeltFullbandStereo20ms, 0x00};
    ProcessCommand command = make_process(0, packet, bytes_of(packet), pcm.data(), 0x1000, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidStream);

    ActivateCommand activate = make_activate(1, 0);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidArg);
    activate = make_activate(1, kMaxChannels + 1);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidArg);
    activate = make_activate(1, 2);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kAlreadyActive);

    command = make_process(1, nullptr, bytes_of(packet), pcm.data(), 0x1000, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidArg);
    command = make_process(1, packet, bytes_of(packet), nullptr, 0x1000, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidArg);
    command = make_process(1, packet, 0, pcm.data(), 0x1000, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidArg);

    const std::uint8_t odd_double[] = {0xF5, 0x00, 0x00, 0x00};
    const std::uint8_t long_first_frame[] = {0xF6, 0xFF, 0xFF, 0x00};
    const std::uint8_t no_frames[] = {0xF7, 0x00, 0x00};
    const std::uint8_t trailing_padding[] = {0xF7, 0xC1, 0xFF};
    const std::uint8_t three_long_frames[] = {0x1B, 0x03, 0x00, 0x00, 0x00};
    const std::uint8_t* const bad_packets[] = {odd_double, long_first_frame, no_frames, trailing_padding,
                                               three_long_frames};
    const std::uint32_t bad_lengths[] = {bytes_of(odd_double), bytes_of(long_first_frame), bytes_of(no_frames),
                                         bytes_of(trailing_padding), bytes_of(three_long_frames)};
    for (std::size_t i = 0; i < std::size(bad_packets); ++i) {
        command = make_process(1, bad_packets[i], bad_lengths[i], pcm.data(), 0x1000, 960);
        CHECK(enqueue_command(handle, &command, nullptr) == kOk);
        CHECK(drain(handle).error == kBadPacket);
    }

    const std::uint8_t two_long_frames[] = {0x1A, 0x00, 0x00};
    command = make_process(1, two_long_frames, bytes_of(two_long_frames), pcm.data(), 0x10000, 5760);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    Result result = drain(handle);
    CHECK(result.error == kOk);
    CHECK(result.frames == kMaxFrames);
    CHECK(result.output_length == kMaxFrames * 8);

    CommandId deactivate = make_id(kDeactivate, 1);
    CHECK(enqueue_command(handle, &deactivate, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);
    command = make_process(1, packet, bytes_of(packet), pcm.data(), 0x1000, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    CHECK(drain(handle).error == kInvalidStream);

    CommandId reset = make_id(kReset, 1);
    CHECK(enqueue_command(handle, &reset, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);
    CommandId notify = make_id(kNotify, 1);
    CHECK(enqueue_command(handle, &notify, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);
    CHECK(disconnect(handle) == kOk);
}

TEST(xapu_keeps_the_frames_the_title_asks_for) {
    const ConnectInput input{kDecodeOpus, 1, 8, 0x4000, 0, kDeinterleavedOutput};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);
    ActivateCommand activate = make_activate(0, 2);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);

    const std::uint8_t packet[] = {kCeltFullbandStereo20ms, 0x00};
    std::vector<std::uint8_t> pcm(0x10000);
    ProcessCommand command = make_process(0, packet, bytes_of(packet), pcm.data(), 0x10000, 100, 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    Result result = drain(handle);
    CHECK(result.error == kOk);
    CHECK(result.frames == 100);
    CHECK(result.output_length == 100 * 8);
    CHECK(silent(pcm.data(), result.output_length));

    command = make_process(0, packet, bytes_of(packet), pcm.data(), 400, 5000);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    result = drain(handle);
    CHECK(result.frames == 50);
    CHECK(result.output_length == 400);

    command = make_process(0, packet, bytes_of(packet), pcm.data(), 0x10000, 960, 5000);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    result = drain(handle);
    CHECK(result.frames == 0);
    CHECK(result.output_length == 0);

    const std::uint8_t short_packet[] = {kCeltNarrowbandMono2p5ms, 0x00};
    command = make_process(0, short_packet, bytes_of(short_packet), pcm.data(), 0x10000, 960);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    result = drain(handle);
    CHECK(result.frames == 120);
    CHECK(result.output_length == 120 * 8);
    CHECK(disconnect(handle) == kOk);
}

TEST(xapu_accepts_padding_in_a_packet) {
    const ConnectInput input{kDecodeOpus, 1, 2, 0x4000, 0, 0};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);
    ActivateCommand activate = make_activate(0, 1);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(drain(handle).error == kOk);

    std::vector<std::uint8_t> padded(258, 0);
    padded[0] = 0xF7;
    padded[1] = 0x41;
    padded[2] = 0xFF;
    std::vector<std::uint8_t> pcm(480 * 4);
    ProcessCommand command = make_process(0, padded.data(), static_cast<std::uint32_t>(padded.size()), pcm.data(),
                                          static_cast<std::uint32_t>(pcm.size()), 480);
    CHECK(enqueue_command(handle, &command, nullptr) == kOk);
    const Result result = drain(handle);
    CHECK(result.error == kOk);
    CHECK(result.frames == 480);
    CHECK(result.output_length == 480 * 4);
    CHECK(disconnect(handle) == kOk);
}

TEST(xapu_signals_the_result_event) {
    const ConnectInput input{kDecodeOpus, 1, 4, 0x4000, 0, kResultEvent};
    ConnectOutput output{};
    Connection* handle = nullptr;
    CHECK(connect(&input, &output, &handle) == kOk);
    CHECK(output.result_event != nullptr);
    HANDLE event = static_cast<HANDLE>(output.result_event);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);

    ActivateCommand activate = make_activate(0, 1);
    CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);

    CommandId notify = make_id(kNotify, 0, kSignal);
    CHECK(enqueue_command(handle, &notify, nullptr) == kOk);
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    CHECK(disconnect(handle) == kOk);
}
