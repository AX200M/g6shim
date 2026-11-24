// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "dsp.hpp"

#include <cstring>
#include <vector>

using namespace g6shim::chad;

namespace {

StreamDesc desc(std::uint32_t kind, std::uint32_t options, std::uint32_t frames, std::uint32_t channels,
                const float* ir) {
    return StreamDesc{kind, options, frames, channels, 0x800, ir};
}

}  // namespace

TEST(chad_stream_as_the_title_drives_it) {
    std::vector<unsigned char> base(0x100000);
    Dsp* dsp = nullptr;
    CHECK(connect(base.data(), 0x100000, 24, 0x80, 1, &dsp) == S_OK);

    std::vector<float> ir(0x800, 0.0f);
    const StreamDesc stereo = desc(kConvolution, 2, 0x400, 2, ir.data());
    const StreamStatus* status = nullptr;
    Stream* stream = nullptr;
    CHECK(activate(dsp, &stereo, &status, &stream) == S_OK);
    CHECK(status != nullptr && stream != nullptr);

    std::vector<float> output(0x800, 1.0f);
    std::vector<float> input(0x800, 0.5f);
    const Command command{{1.0f, 1.0f}, input.data(), output.data(), 0};
    std::uint32_t ticket = 0;
    CHECK(submit_command(stream, &command, &ticket) == S_OK);
    CHECK(ticket == 1);
    CHECK(status->completed.load(std::memory_order_acquire) == ticket);
    CHECK(status->result.load(std::memory_order_acquire) == S_OK);
    bool silent = true;
    for (const float sample : output) silent = silent && sample == 0.0f;
    CHECK(silent);
    CHECK(deactivate(stream) == S_OK);

    const StreamDesc mono_stereo_options = desc(kConvolution, 2, 0x400, 1, ir.data());
    CHECK(activate(dsp, &mono_stereo_options, &status, &stream) == E_INVALIDARG);

    const StreamDesc transform = desc(2, 0, 0x400, 1, nullptr);
    CHECK(activate(dsp, &transform, &status, &stream) == S_OK);
    CHECK(deactivate(stream) == S_OK);
    CHECK(disconnect(dsp) == S_OK);
}

TEST(chad_connect_rejects_out_of_range_requests) {
    std::vector<unsigned char> base(kMinBaseLength);
    Dsp* dsp = nullptr;
    CHECK(connect(nullptr, kMinBaseLength, 24, 0x80, 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMinBaseLength - 1, 24, 0x80, 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMaxBaseLength + 1, 24, 0x80, 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMinBaseLength, kMaxIrSeconds + 1, 0x80, 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMinBaseLength, 24, kMaxStreams + 1, 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMinBaseLength, 24, 0x80, kMaxChannels + 1, &dsp) == E_INVALIDARG);
    CHECK(connect(base.data(), kMinBaseLength, 24, 0x80, 1, nullptr) == E_POINTER);
    CHECK(connect(base.data(), kMinBaseLength, kMaxIrSeconds, kMaxStreams, kMaxChannels, &dsp) == S_OK);
    CHECK(dsp != nullptr);
    CHECK(disconnect(dsp) == S_OK);
    CHECK(disconnect(nullptr) == E_INVALIDARG);
}

TEST(chad_activate_checks_shape_and_stream_budget) {
    std::vector<unsigned char> base(kMinBaseLength);
    Dsp* dsp = nullptr;
    CHECK(connect(base.data(), kMinBaseLength, 8, 1, 2, &dsp) == S_OK);

    std::vector<float> ir(4, 0.0f);
    const StreamStatus* status = nullptr;
    Stream* stream = nullptr;
    CHECK(!stream_shape_valid(desc(kConvolution, kNotForConvolution, 0x400, 2, ir.data())));
    CHECK(!stream_shape_valid(desc(kConvolution, 0, 0x800, 2, ir.data())));
    CHECK(!stream_shape_valid(desc(kConvolution, 0, 0x400, 3, ir.data())));
    CHECK(!stream_shape_valid(desc(2, 0, 0x1000, 1, nullptr)));
    CHECK(!stream_shape_valid(desc(2, 0, 0x400, 2, nullptr)));
    CHECK(!stream_shape_valid(desc(2, kStereoOptions, 0x400, 1, nullptr)));
    CHECK(stream_shape_valid(desc(kConvolution, 0, 0x200, 1, ir.data())));
    CHECK(stream_shape_valid(desc(kConvolution, kStereoOptions, 0x400, 2, ir.data())));
    CHECK(stream_shape_valid(desc(2, 0, 0x800, 1, nullptr)));

    const StreamDesc stereo = desc(kConvolution, 2, 0x400, 2, ir.data());
    CHECK(activate(dsp, &stereo, nullptr, &stream) == E_POINTER);
    CHECK(activate(dsp, &stereo, &status, nullptr) == E_POINTER);
    CHECK(activate(nullptr, &stereo, &status, &stream) == E_INVALIDARG);
    CHECK(activate(dsp, nullptr, &status, &stream) == E_INVALIDARG);
    CHECK(activate(dsp, &stereo, &status, &stream) == S_OK);
    CHECK(activate(dsp, &stereo, &status, &stream) == E_OUTOFMEMORY);
    CHECK(deactivate(stream) == S_OK);
    CHECK(activate(dsp, &stereo, &status, &stream) == S_OK);
    CHECK(deactivate(stream) == S_OK);
    CHECK(deactivate(nullptr) == E_INVALIDARG);
    CHECK(disconnect(dsp) == S_OK);
}

TEST(chad_submit_completes_without_an_output_block) {
    std::vector<unsigned char> base(kMinBaseLength);
    Dsp* dsp = nullptr;
    CHECK(connect(base.data(), kMinBaseLength, 8, 0x80, 1, &dsp) == S_OK);

    std::vector<float> ir(4, 0.0f);
    const StreamDesc mono = desc(kConvolution, 0, 0x200, 1, ir.data());
    const StreamStatus* status = nullptr;
    Stream* stream = nullptr;
    CHECK(activate(dsp, &mono, &status, &stream) == S_OK);

    std::vector<float> input(0x200, 0.5f);
    const Command silent{{1.0f, 1.0f}, input.data(), nullptr, 0};
    std::uint32_t ticket = 0;
    CHECK(submit_command(stream, &silent, &ticket) == S_OK);
    CHECK(ticket == 1);
    CHECK(submit_command(stream, &silent, nullptr) == S_OK);
    CHECK(status->completed.load(std::memory_order_acquire) == 2);
    CHECK(status->result.load(std::memory_order_acquire) == S_OK);
    CHECK(submit_command(nullptr, &silent, &ticket) == E_INVALIDARG);
    CHECK(submit_command(stream, nullptr, &ticket) == E_INVALIDARG);
    CHECK(deactivate(stream) == S_OK);
    CHECK(disconnect(dsp) == S_OK);
}
