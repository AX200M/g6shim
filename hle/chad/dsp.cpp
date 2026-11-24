// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp.hpp"

#include <g6shim/log.hpp>

#include <cstring>
#include <new>

namespace g6shim::chad {

namespace {

constexpr std::uint32_t kConvolutionFrames = 0x200;
constexpr std::uint32_t kConvolutionBlockFrames = 0x400;
constexpr std::uint32_t kTransformBlockFrames = 0x800;
constexpr std::uint32_t kMonoChannels = 1;

bool convolution_shape(const StreamDesc& desc) {
    return desc.channels >= kMonoChannels && desc.channels <= kMaxChannels
           && (desc.frames == kConvolutionFrames || desc.frames == kConvolutionBlockFrames)
           && (desc.options & kNotForConvolution) == 0;
}

bool transform_shape(const StreamDesc& desc) {
    return desc.channels == kMonoChannels
           && (desc.frames == kConvolutionFrames || desc.frames == kConvolutionBlockFrames
               || desc.frames == kTransformBlockFrames);
}

}  // namespace

bool stream_shape_valid(const StreamDesc& desc) {
    const bool shape = desc.kind == kConvolution ? convolution_shape(desc) : transform_shape(desc);
    return shape && ((desc.options & kStereoOptions) == 0 || desc.channels == kMaxChannels);
}

HRESULT connect(void* base, std::uint32_t base_length, std::uint32_t ir_seconds, std::uint32_t max_streams,
                std::uint32_t max_channels, Dsp** out) {
    if (!base || base_length < kMinBaseLength || base_length > kMaxBaseLength || ir_seconds > kMaxIrSeconds
        || max_streams > kMaxStreams || max_channels > kMaxChannels)
        return E_INVALIDARG;
    if (!out) return E_POINTER;
    auto* dsp = new (std::nothrow) Dsp{max_streams};
    if (!dsp) return E_OUTOFMEMORY;
    *out = dsp;
    G6SHIM_INFO("XDspConnectWithMaximumStreamLimit(base %p, length 0x%X, ir %u s, streams %u, channels %u) -> 0x%08lX",
                base, base_length, ir_seconds, max_streams, max_channels, static_cast<unsigned long>(S_OK));
    return S_OK;
}

HRESULT disconnect(Dsp* dsp) {
    if (!dsp) return E_INVALIDARG;
    delete dsp;
    return S_OK;
}

HRESULT activate(Dsp* dsp, const StreamDesc* desc, const StreamStatus** status, Stream** stream) {
    if (!status || !stream) return E_POINTER;
    if (!dsp || !desc) return E_INVALIDARG;
    if (!stream_shape_valid(*desc)) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> held(dsp->mutex);
        if (dsp->active >= dsp->max_streams) return E_OUTOFMEMORY;
        ++dsp->active;
    }
    auto* fresh = new (std::nothrow) Stream{dsp, desc->frames, desc->channels};
    if (!fresh) {
        std::lock_guard<std::mutex> held(dsp->mutex);
        --dsp->active;
        return E_OUTOFMEMORY;
    }
    *status = &fresh->status;
    *stream = fresh;
    G6SHIM_TRACE("XDspActivate(kind %u, options 0x%X, frames %u, channels %u) -> 0x%08lX", desc->kind, desc->options,
                 desc->frames, desc->channels, static_cast<unsigned long>(S_OK));
    return S_OK;
}

HRESULT deactivate(Stream* stream) {
    if (!stream) return E_INVALIDARG;
    Dsp* dsp = stream->dsp;
    delete stream;
    std::lock_guard<std::mutex> held(dsp->mutex);
    --dsp->active;
    return S_OK;
}

HRESULT submit_command(Stream* stream, const Command* command, std::uint32_t* ticket) {
    if (!stream || !command) return E_INVALIDARG;
    if (command->output) {
        const std::size_t samples = static_cast<std::size_t>(stream->frames) * stream->channels;
        std::memset(command->output, 0, samples * sizeof(float));
    }
    StreamStatus& status = stream->status;
    const std::uint32_t next = status.submitted.load(std::memory_order_relaxed) + 1;
    status.submitted.store(next, std::memory_order_relaxed);
    status.result.store(S_OK, std::memory_order_relaxed);
    status.completed.store(next, std::memory_order_release);
    if (ticket) *ticket = next;
    return S_OK;
}

}  // namespace g6shim::chad

using namespace g6shim::chad;

extern "C" HRESULT g6shim_XDspConnectWithMaximumStreamLimit(void* base, std::uint32_t base_length,
                                                           std::uint32_t ir_seconds, std::uint32_t max_streams,
                                                           std::uint32_t max_channels, Dsp** out) {
    return connect(base, base_length, ir_seconds, max_streams, max_channels, out);
}

extern "C" HRESULT g6shim_XDspDisconnect(Dsp* dsp) { return disconnect(dsp); }

extern "C" HRESULT g6shim_XDspActivate(Dsp* dsp, const StreamDesc* desc, const StreamStatus** status,
                                       Stream** stream) {
    return activate(dsp, desc, status, stream);
}

extern "C" HRESULT g6shim_XDspDeactivate(Stream* stream) { return deactivate(stream); }

extern "C" HRESULT g6shim_XDspSubmitCommand(Stream* stream, const Command* command, std::uint32_t* ticket) {
    return submit_command(stream, command, ticket);
}
