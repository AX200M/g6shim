// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "speech.hpp"

#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <array>
#include <cstring>
#include <string>

namespace g6shim::xgameruntime::speech {

namespace {

using This = Object*;
using Handle = const void*;

std::uint8_t kSynthesizer = 0;
std::uint8_t kStream = 0;

HResult enumerate_voices(This, void*, const void*) { return kOk; }

HResult create(This, Handle* out) { return put(out, static_cast<Handle>(&kSynthesizer)); }

HResult close(This, Handle) { return kOk; }

HResult set_default_voice(This, Handle) { return kOk; }

HResult set_custom_voice(This, Handle, const char*) { return kOk; }

HResult create_stream(This, Handle, const char* words, Handle* out) {
    G6SHIM_TRACE("xgameruntime: speech (silent): %s", text(words).value_or("").c_str());
    return put(out, static_cast<Handle>(&kStream));
}

HResult stream_size(This, Handle, std::size_t* out) { return put(out, kSilence.size()); }

HResult stream_data(This, Handle, std::size_t size, std::uint8_t* buffer, std::size_t* used) {
    if (buffer == nullptr || size < kSilence.size()) return kNotSufficientBuffer;
    std::memcpy(buffer, kSilence.data(), kSilence.size());
    if (used != nullptr) *used = kSilence.size();
    return kOk;
}

const std::array<const void*, 22> kVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&missing<3>),
    slot(&missing<4>),
    slot(&missing<5>),
    slot(&missing<6>),
    slot(&missing<7>),
    slot(&enumerate_voices),
    slot(&create),
    slot(&close),
    slot(&set_default_voice),
    slot(&set_custom_voice),
    slot(&create_stream),
    slot(&close),
    slot(&stream_size),
    slot(&stream_data),
    slot(&missing<17>),
    slot(&missing<18>),
    slot(&missing<19>),
    slot(&missing<20>),
    slot(&create_stream),
};

}  // namespace

const Object kObject{kVtable.data(), "accessibility"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kIid, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::speech

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::speech_interfaces() {
    return speech::interfaces();
}
