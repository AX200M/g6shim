// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "apu.hpp"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

#include <opus.h>

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

Result drain(Connection* handle) {
    Result result{};
    CHECK(dequeue_result(handle, &result) == kOk);
    return result;
}

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kFrames20ms = 960;
constexpr std::size_t kMaxPacketBytes = 4000;
constexpr std::size_t kTwoByteLengthFrom = 252;
constexpr std::uint8_t kFramingMask = 3;
constexpr std::uint8_t kTwoFramesVariable = 2;
constexpr std::uint8_t kArbitraryFrames = 3;

struct EncoderDeleter {
    void operator()(OpusEncoder* encoder) const { opus_encoder_destroy(encoder); }
};

struct DecoderDeleter {
    void operator()(OpusDecoder* decoder) const { opus_decoder_destroy(decoder); }
};

using Encoder = std::unique_ptr<OpusEncoder, EncoderDeleter>;
using Reference = std::unique_ptr<OpusDecoder, DecoderDeleter>;

Encoder make_encoder(std::uint32_t channels, int application = OPUS_APPLICATION_AUDIO) {
    int error = OPUS_OK;
    Encoder encoder(
        opus_encoder_create(static_cast<opus_int32>(kSampleRate), static_cast<int>(channels), application, &error));
    CHECK(error == OPUS_OK);
    return encoder;
}

Reference make_reference(std::uint32_t channels) {
    int error = OPUS_OK;
    Reference decoder(opus_decoder_create(static_cast<opus_int32>(kSampleRate), static_cast<int>(channels), &error));
    CHECK(error == OPUS_OK);
    return decoder;
}

std::vector<float> tone(std::uint32_t frames, std::uint32_t channels, std::uint32_t start, float amplitude = 0.5f) {
    std::vector<float> pcm(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const double hertz = 440.0 * (channel + 1);
            pcm[static_cast<std::size_t>(frame) * channels + channel] =
                amplitude * static_cast<float>(std::sin(2.0 * kPi * hertz * (start + frame) / kSampleRate));
        }
    return pcm;
}

std::vector<std::uint8_t> encode(OpusEncoder* encoder, const std::vector<float>& pcm, std::uint32_t frames) {
    std::vector<std::uint8_t> packet(kMaxPacketBytes);
    const opus_int32 bytes = opus_encode_float(encoder, pcm.data(), static_cast<int>(frames), packet.data(),
                                               static_cast<opus_int32>(packet.size()));
    CHECK(bytes > 0);
    packet.resize(bytes > 0 ? static_cast<std::size_t>(bytes) : 0);
    return packet;
}

std::vector<std::uint8_t> join(const std::vector<std::vector<std::uint8_t>>& parts) {
    std::unique_ptr<OpusRepacketizer, void (*)(OpusRepacketizer*)> joiner(opus_repacketizer_create(),
                                                                           &opus_repacketizer_destroy);
    CHECK(joiner != nullptr);
    std::size_t capacity = 0;
    for (const std::vector<std::uint8_t>& part : parts) {
        CHECK(opus_repacketizer_cat(joiner.get(), part.data(), static_cast<opus_int32>(part.size())) == OPUS_OK);
        capacity += part.size() + 2;
    }
    std::vector<std::uint8_t> packet(capacity + 2);
    const opus_int32 bytes = opus_repacketizer_out(joiner.get(), packet.data(), static_cast<opus_int32>(packet.size()));
    CHECK(bytes > 0);
    packet.resize(bytes > 0 ? static_cast<std::size_t>(bytes) : 0);
    return packet;
}

std::vector<std::vector<std::uint8_t>> tone_packets(std::uint32_t channels, std::size_t count) {
    Encoder encoder = make_encoder(channels);
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::size_t i = 0; i < count; ++i)
        packets.push_back(encode(encoder.get(), tone(kFrames20ms, channels, static_cast<std::uint32_t>(i) * kFrames20ms),
                                 kFrames20ms));
    return packets;
}

std::vector<float> reference_decode(OpusDecoder* decoder, const std::vector<std::uint8_t>& packet,
                                    std::uint32_t channels) {
    std::vector<float> pcm(static_cast<std::size_t>(kMaxFrames) * channels);
    const int frames = opus_decode_float(decoder, packet.data(), static_cast<opus_int32>(packet.size()), pcm.data(),
                                         static_cast<int>(kMaxFrames), 0);
    CHECK(frames > 0);
    pcm.resize(frames > 0 ? static_cast<std::size_t>(frames) * channels : 0);
    return pcm;
}

std::vector<float> as_floats(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    std::vector<float> floats(count);
    std::memcpy(floats.data(), bytes.data(), count * sizeof(float));
    return floats;
}

float peak(const std::vector<float>& pcm) {
    float loudest = 0.0f;
    for (const float sample : pcm) loudest = std::fmax(loudest, std::fabs(sample));
    return loudest;
}

struct Session {
    Connection* handle = nullptr;

    Session(std::uint32_t channels, std::uint32_t options) {
        const ConnectInput input{kDecodeOpus, 1, kMaxCommandsPerStream, 0x1000, 0, options};
        ConnectOutput output{};
        CHECK(connect(&input, &output, &handle) == kOk);
        ActivateCommand activate = make_activate(0, channels);
        CHECK(enqueue_command(handle, &activate, nullptr) == kOk);
        CHECK(drain(handle).error == kOk);
    }
    ~Session() { disconnect(handle); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Result run(const std::vector<std::uint8_t>& packet, std::vector<std::uint8_t>& pcm, std::uint32_t frame_count,
               std::uint32_t first_frame = 0) {
        ProcessCommand command =
            make_process(0, packet.data(), static_cast<std::uint32_t>(packet.size()), pcm.data(),
                         static_cast<std::uint32_t>(pcm.size()), frame_count, first_frame);
        CHECK(enqueue_command(handle, &command, nullptr) == kOk);
        return drain(handle);
    }

    void command(std::uint32_t kind) {
        CommandId id = make_id(kind, 0);
        CHECK(enqueue_command(handle, &id, nullptr) == kOk);
        CHECK(drain(handle).error == kOk);
    }
};

std::vector<std::uint8_t> output_buffer(std::uint32_t channels) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(kMaxFrames) * channels * sizeof(float));
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

TEST(xapu_decodes_opus_packets) {
    for (const std::uint32_t channels : {1u, 2u}) {
        const std::vector<std::vector<std::uint8_t>> packets = tone_packets(channels, 4);
        Session session(channels, 0);
        Reference reference = make_reference(channels);
        std::vector<std::uint8_t> pcm = output_buffer(channels);
        for (const std::vector<std::uint8_t>& packet : packets) {
            const Result result = session.run(packet, pcm, kMaxFrames);
            CHECK(result.error == kOk);
            CHECK(result.frames == kFrames20ms);
            CHECK(result.output_length == kFrames20ms * channels * sizeof(float));
            const std::vector<float> expected = reference_decode(reference.get(), packet, channels);
            CHECK(as_floats(pcm, expected.size()) == expected);
        }
        CHECK(peak(as_floats(pcm, static_cast<std::size_t>(kFrames20ms) * channels)) > 0.1f);
    }
}

TEST(xapu_decodes_packets_of_several_frames) {
    constexpr std::uint32_t kChannels = 2;
    constexpr opus_int32 kBitrates[] = {128000, 64000, 96000};
    Encoder encoder = make_encoder(kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    CHECK(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)) == OPUS_OK);
    CHECK(opus_encoder_ctl(encoder.get(), OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)) == OPUS_OK);
    CHECK(opus_encoder_ctl(encoder.get(), OPUS_SET_FORCE_CHANNELS(2)) == OPUS_OK);
    std::vector<std::vector<std::uint8_t>> parts;
    for (std::uint32_t i = 0; i < std::size(kBitrates); ++i) {
        CHECK(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(kBitrates[i])) == OPUS_OK);
        parts.push_back(encode(encoder.get(), tone(kFrames20ms, kChannels, i * kFrames20ms), kFrames20ms));
    }
    CHECK(parts[0].size() >= kTwoByteLengthFrom);

    const std::vector<std::uint8_t> two = join({parts[0], parts[1]});
    const std::vector<std::uint8_t> three = join(parts);
    CHECK((two[0] & kFramingMask) == kTwoFramesVariable);
    CHECK((three[0] & kFramingMask) == kArbitraryFrames);

    Session session(kChannels, 0);
    Reference reference = make_reference(kChannels);
    std::vector<std::uint8_t> pcm = output_buffer(kChannels);
    const struct {
        const std::vector<std::uint8_t>* packet;
        std::uint32_t frames;
    } cases[] = {{&two, 2 * kFrames20ms}, {&three, 3 * kFrames20ms}};
    for (const auto& item : cases) {
        const Result result = session.run(*item.packet, pcm, kMaxFrames);
        CHECK(result.error == kOk);
        CHECK(result.frames == item.frames);
        const std::vector<float> expected = reference_decode(reference.get(), *item.packet, kChannels);
        CHECK(expected.size() == static_cast<std::size_t>(item.frames) * kChannels);
        CHECK(as_floats(pcm, expected.size()) == expected);
    }
}

TEST(xapu_writes_deinterleaved_planes) {
    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint32_t kFirst = 100;
    constexpr std::uint32_t kKept = 300;
    const std::vector<std::vector<std::uint8_t>> packets = tone_packets(kChannels, 3);
    Session interleaved(kChannels, 0);
    Session planar(kChannels, kDeinterleavedOutput);
    Reference reference = make_reference(kChannels);
    std::vector<std::uint8_t> mixed = output_buffer(kChannels);
    std::vector<std::uint8_t> planes = output_buffer(kChannels);
    for (const std::vector<std::uint8_t>& packet : packets) {
        const Result a = interleaved.run(packet, mixed, kKept, kFirst);
        const Result b = planar.run(packet, planes, kKept, kFirst);
        CHECK(a.error == kOk);
        CHECK(b.error == kOk);
        CHECK(a.frames == kKept);
        CHECK(b.frames == kKept);
        CHECK(b.output_length == kKept * kChannels * sizeof(float));

        const std::vector<float> decoded = reference_decode(reference.get(), packet, kChannels);
        const std::vector<float> expected(decoded.begin() + kFirst * kChannels,
                                          decoded.begin() + (kFirst + kKept) * kChannels);
        const std::vector<float> samples = as_floats(mixed, expected.size());
        CHECK(samples == expected);

        const std::vector<float> split = as_floats(planes, expected.size());
        bool transposed = true;
        for (std::uint32_t frame = 0; frame < kKept; ++frame)
            for (std::uint32_t channel = 0; channel < kChannels; ++channel)
                transposed = transposed && split[channel * kKept + frame] == samples[frame * kChannels + channel];
        CHECK(transposed);
    }
}

TEST(xapu_reset_returns_a_stream_to_a_fresh_decoder) {
    constexpr std::uint32_t kChannels = 2;
    const std::vector<std::vector<std::uint8_t>> packets = tone_packets(kChannels, 3);
    Reference fresh = make_reference(kChannels);
    const std::vector<float> expected = reference_decode(fresh.get(), packets[2], kChannels);
    Session session(kChannels, 0);
    std::vector<std::uint8_t> pcm = output_buffer(kChannels);
    for (const std::vector<std::uint8_t>& packet : packets) CHECK(session.run(packet, pcm, kMaxFrames).error == kOk);
    CHECK(as_floats(pcm, expected.size()) != expected);

    session.command(kReset);
    CHECK(session.run(packets[2], pcm, kMaxFrames).error == kOk);
    CHECK(as_floats(pcm, expected.size()) == expected);
}

TEST(xapu_survives_damaged_packets) {
    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint32_t kMaxAppended = 64;
    const std::vector<std::vector<std::uint8_t>> packets = tone_packets(kChannels, 4);
    Session session(kChannels, kDeinterleavedOutput);
    std::vector<std::uint8_t> pcm = output_buffer(kChannels);
    std::mt19937 rng(0x0A75u);
    std::size_t decoded = 0;
    std::size_t refused = 0;
    for (std::uint32_t round = 0; round < 3000; ++round) {
        std::vector<std::uint8_t> packet = packets[round % packets.size()];
        const std::uint32_t edits = 1 + static_cast<std::uint32_t>(rng() % 4);
        for (std::uint32_t edit = 0; edit < edits; ++edit) {
            switch (rng() % 4) {
                case 0: {
                    std::uint8_t& byte = packet[rng() % packet.size()];
                    byte = static_cast<std::uint8_t>(byte ^ (1u << (rng() % 8)));
                    break;
                }
                case 1:
                    packet.resize(1 + rng() % packet.size());
                    break;
                case 2:
                    for (std::uint32_t extra = rng() % kMaxAppended; extra > 0; --extra)
                        packet.push_back(static_cast<std::uint8_t>(rng()));
                    break;
                default:
                    packet[0] = static_cast<std::uint8_t>(rng());
                    break;
            }
        }
        const Result result = session.run(packet, pcm, kMaxFrames);
        CHECK(result.error == kOk || result.error == kBadPacket);
        if (result.error == kOk) {
            CHECK(result.frames <= kMaxFrames);
            CHECK(result.output_length == result.frames * kChannels * sizeof(float));
            ++decoded;
        } else {
            CHECK(result.frames == 0);
            ++refused;
        }
    }
    CHECK(decoded > 0);
    CHECK(refused > 0);
}
