// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xma_voices.hpp"

#include "apu_memory.hpp"
#include "xma.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>

namespace g6shim::acphal {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kContextBytes = 64;
constexpr std::size_t kBlockBytes = 256;
constexpr std::size_t kSamplesPerBlock = kBlockBytes / 2;
constexpr std::uint32_t kInputInvalid = 4;
constexpr std::uint32_t kFrameRates[4] = {24000, 32000, 44100, 48000};
constexpr LONGLONG kDecoderInterval100ns = 20000;
constexpr std::uint32_t kHeaderBits = 32;
constexpr std::uint32_t kInputPacketAlignment = 0x800;

struct Trace {
    Clock::time_point since;
    std::size_t produced = 0;
    std::size_t taken = 0;
    std::uint32_t takes = 0;
    std::uint32_t partial_takes = 0;
    std::uint32_t frames = 0;
    std::uint32_t bad_frames = 0;
    std::int16_t peak = 0;
    std::optional<std::uint64_t> ended_ms;
};

struct Voice {
    std::unique_ptr<xma::Decoder> decoder;
    std::deque<std::int16_t> pcm;
    std::size_t skip = 0;
    bool ended = false;
    Trace trace;
};

struct VoiceSlot {
    std::mutex mutex;
    std::unique_ptr<Voice> voice;
};

struct State {
    std::uint8_t* array = nullptr;
    std::vector<std::unique_ptr<VoiceSlot>> voices;
};

struct Context {
    std::uint32_t* words;

    std::uint32_t get(std::size_t word) const {
        return *reinterpret_cast<volatile std::uint32_t*>(words + word);
    }

    void set(std::size_t word, std::uint32_t value) const {
        *reinterpret_cast<volatile std::uint32_t*>(words + word) = value;
    }

    std::uint32_t field(std::size_t word, std::uint32_t shift, std::uint32_t bits) const {
        return (get(word) >> shift) & ((1u << bits) - 1);
    }

    void set_field(std::size_t word, std::uint32_t shift, std::uint32_t bits, std::uint32_t value) const {
        const std::uint32_t mask = ((1u << bits) - 1) << shift;
        set(word, (get(word) & ~mask) | ((value << shift) & mask));
    }

    std::size_t channels() const { return 1 + field(1, 29, 1); }
};

State& state() {
    static State* instance = new State();
    return *instance;
}

std::shared_mutex& state_mutex() {
    static std::shared_mutex* instance = new std::shared_mutex();
    return *instance;
}

HANDLE wake_event() {
    static const HANDLE instance = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return instance;
}

std::once_flag& decoder_once() {
    static std::once_flag* instance = new std::once_flag();
    return *instance;
}

std::optional<Context> context_at(const State& current, std::uint32_t index) {
    if (index >= current.voices.size() || !current.array) return std::nullopt;
    return Context{reinterpret_cast<std::uint32_t*>(current.array + static_cast<std::size_t>(index) * kContextBytes)};
}

std::span<const std::uint8_t> apu_bytes(std::uint32_t apu, std::size_t bytes) {
    const void* base = apu_memory().virtual_address(apu, static_cast<std::uint32_t>(bytes));
    if (!base) return {};
    return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(base), bytes);
}

std::int16_t magnitude(std::int16_t sample) {
    if (sample == INT16_MIN) return INT16_MAX;
    return sample < 0 ? static_cast<std::int16_t>(-sample) : sample;
}

bool decode(Context context, Voice& voice) {
    if (voice.ended) return false;
    const std::size_t channels = context.channels();
    std::uint32_t current = context.field(4, 31, 1);
    const std::uint32_t valid = context.field(0, 20, 2);
    if ((valid & (1u << current)) == 0) {
        if ((valid & (1u << (1 - current))) == 0) {
            if (voice.decoder) {
                std::vector<std::int16_t> tail;
                voice.decoder->flush(tail);
                voice.pcm.insert(voice.pcm.end(), tail.begin(), tail.end());
            }
            voice.ended = true;
            voice.trace.ended_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - voice.trace.since).count());
            context.set_field(2, 26, 5, kInputInvalid);
            context.set_field(2, 31, 1, 1);
            return !voice.pcm.empty();
        }
        current = 1 - current;
        context.set_field(4, 31, 1, current);
        context.set_field(2, 0, 26, 0);
    }
    const std::uint32_t address = current == 0 ? context.get(5) : context.get(6);
    const std::uint32_t packets = current == 0 ? context.field(0, 0, 12) : context.field(1, 0, 12);
    const std::size_t input_bytes = static_cast<std::size_t>(packets) * xma::kPacketBytes;
    const std::span<const std::uint8_t> data = apu_bytes(address & ~(kInputPacketAlignment - 1), input_bytes);
    if (data.size() != input_bytes) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    std::size_t at = context.field(2, 0, 26);
    if (at < kHeaderBits) {
        const auto header = xma::packet(data, 0);
        at = kHeaderBits + (header ? header->first_frame_bit : 0);
    }
    const auto frame = xma::read_frame(data, at);
    if (!frame) {
        context.set_field(0, 20 + current, 1, 0);
        return true;
    }
    if (!voice.decoder) {
        const std::size_t rate = kFrameRates[context.field(1, 27, 2)];
        voice.decoder = std::make_unique<xma::Decoder>(channels, static_cast<std::uint32_t>(rate));
    }
    std::vector<std::int16_t> pcm;
    pcm.reserve(xma::kFrameSamples * channels);
    ++voice.trace.frames;
    if (!voice.decoder->decode(frame->data, frame->bits, pcm)) {
        ++voice.trace.bad_frames;
        if (voice.trace.bad_frames <= 2)
            G6SHIM_TRACE("XMA frame did not decode: bit %zu, length %zu", at, frame->bits);
    }
    std::size_t next = frame->next.value_or(0);
    const std::uint32_t loops = context.field(0, 12, 8);
    const std::size_t loop_end = context.field(4, 0, 26);
    const bool ends_loop = loop_end != 0 &&
                           (frame->start == loop_end ||
                            (frame->start < loop_end && (!frame->next || loop_end < *frame->next)));
    if (loops > 0 && ends_loop) {
        const std::size_t keep =
            (static_cast<std::size_t>(context.field(1, 12, 2)) + 1) * xma::kSubframeSamples * channels;
        if (pcm.size() > keep) pcm.resize(keep);
        next = context.field(3, 0, 26);
        voice.skip = static_cast<std::size_t>(context.field(1, 17, 3)) * xma::kSubframeSamples;
        if (loops != 255) context.set_field(0, 12, 8, loops - 1);
    }
    const std::size_t skip = std::min(voice.skip * channels, pcm.size());
    voice.skip -= skip / channels;
    voice.pcm.insert(voice.pcm.end(), pcm.begin() + skip, pcm.end());
    if (next == 0) context.set_field(0, 20 + current, 1, 0);
    context.set_field(2, 0, 26, static_cast<std::uint32_t>(next));
    return true;
}

void pump_voice(Context context, Voice& voice) {
    const std::uint32_t size = context.field(0, 22, 5);
    if (size == 0) return;
    while (context.field(1, 31, 1) == 1) {
        if (voice.pcm.size() < kSamplesPerBlock && !decode(context, voice)) return;
        const std::uint32_t write = context.field(0, 27, 5);
        const std::uint32_t address = context.get(7) + write * static_cast<std::uint32_t>(kBlockBytes);
        auto* out = static_cast<std::uint8_t*>(
            apu_memory().virtual_address(address, static_cast<std::uint32_t>(kBlockBytes)));
        if (!out) return;
        for (std::size_t i = 0; i < kSamplesPerBlock; ++i) {
            const std::int16_t sample = voice.pcm.empty() ? 0 : voice.pcm.front();
            if (!voice.pcm.empty()) voice.pcm.pop_front();
            voice.trace.peak = std::max(voice.trace.peak, magnitude(sample));
            const auto value = static_cast<std::uint16_t>(sample);
            out[2 * i] = static_cast<std::uint8_t>(value & 0xff);
            out[2 * i + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        voice.trace.produced += kSamplesPerBlock / context.channels();
        const std::uint32_t next_write = (write + 1) % size;
        context.set_field(0, 27, 5, next_write);
        if (next_write == context.field(9, 0, 5)) context.set_field(1, 31, 1, 0);
    }
}

void decoder_loop() {
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        xma_pump();
        decoder_wait(timer, wake_event());
    }
}

void wake_decoder() {
    std::call_once(decoder_once(), [] { std::thread(decoder_loop).detach(); });
    SetEvent(wake_event());
}

}  // namespace

void decoder_wait(HANDLE timer, HANDLE wake) {
    if (timer == nullptr) {
        WaitForSingleObject(wake, 1);
        return;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -kDecoderInterval100ns;
    if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(wake, 1);
        return;
    }
    const HANDLE handles[2] = {timer, wake};
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
}

void set_xma_context_array(std::uint8_t* array, std::uint32_t voices) {
    std::unique_lock<std::shared_mutex> lock(state_mutex());
    State& current = state();
    current.voices.clear();
    current.array = array;
    current.voices.reserve(voices);
    for (std::uint32_t i = 0; i < voices; ++i) current.voices.push_back(std::make_unique<VoiceSlot>());
}

void clear_xma_context_array() {
    std::unique_lock<std::shared_mutex> lock(state_mutex());
    State& current = state();
    current.array = nullptr;
    current.voices.clear();
}

std::uint32_t xma_voice_count() {
    std::shared_lock<std::shared_mutex> lock(state_mutex());
    return static_cast<std::uint32_t>(state().voices.size());
}

void xma_enable(std::uint32_t index) {
    bool enabled = false;
    {
        std::shared_lock<std::shared_mutex> lock(state_mutex());
        const auto context = context_at(state(), index);
        if (!context) return;
        VoiceSlot& slot = *state().voices[index];
        std::lock_guard<std::mutex> voice_lock(slot.mutex);
        context->set_field(2, 26, 6, 0);
        G6SHIM_TRACE("XMA voice %lu enabled", index);
        slot.voice = std::make_unique<Voice>();
        slot.voice->skip = static_cast<std::size_t>(context->field(1, 24, 3)) * xma::kSubframeSamples;
        slot.voice->trace.since = Clock::now();
        enabled = true;
    }
    if (enabled) wake_decoder();
}

void xma_disable(std::uint32_t index) {
    std::shared_lock<std::shared_mutex> lock(state_mutex());
    const auto context = context_at(state(), index);
    if (!context) return;
    VoiceSlot& slot = *state().voices[index];
    std::lock_guard<std::mutex> voice_lock(slot.mutex);
    if (!slot.voice) return;
    const std::unique_ptr<Voice> voice = std::move(slot.voice);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - voice->trace.since).count();
    G6SHIM_TRACE("XMA voice %lu disabled after %lld ms: %zu produced, %zu taken, %u frames, %u bad", index,
                 static_cast<long long>(elapsed), voice->trace.produced,
                 voice->trace.taken * kSamplesPerBlock / context->channels(), voice->trace.frames,
                 voice->trace.bad_frames);
}

void xma_advance_read(std::uint32_t index, std::uint32_t blocks) {
    bool advanced = false;
    {
        std::shared_lock<std::shared_mutex> lock(state_mutex());
        const auto context = context_at(state(), index);
        if (!context) return;
        VoiceSlot& slot = *state().voices[index];
        std::lock_guard<std::mutex> voice_lock(slot.mutex);
        const std::uint32_t size = std::max(context->field(0, 22, 5), 1u);
        const std::uint32_t read = (context->field(9, 0, 5) + blocks % size) % size;
        context->set_field(9, 0, 5, read);
        if (blocks > 0) context->set_field(1, 31, 1, 1);
        if (slot.voice) {
            slot.voice->trace.taken += blocks;
            ++slot.voice->trace.takes;
            slot.voice->trace.partial_takes += blocks < size ? 1u : 0u;
        }
        advanced = true;
    }
    if (advanced) wake_decoder();
}

void xma_pump() {
    const Clock::time_point start = Clock::now();
    std::size_t active = 0;
    {
        std::shared_lock<std::shared_mutex> lock(state_mutex());
        State& current = state();
        for (std::uint32_t index = 0; index < current.voices.size(); ++index) {
            const auto context = context_at(current, index);
            if (!context) continue;
            VoiceSlot& slot = *current.voices[index];
            std::lock_guard<std::mutex> voice_lock(slot.mutex);
            if (!slot.voice) continue;
            pump_voice(*context, *slot.voice);
            ++active;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (elapsed >= 4)
        G6SHIM_TRACE("slow XMA decoding pass: %zu voices, %lld ms", active, static_cast<long long>(elapsed));
}

std::optional<std::size_t> xma_produced(std::uint32_t index) {
    std::shared_lock<std::shared_mutex> lock(state_mutex());
    const State& current = state();
    if (index >= current.voices.size()) return std::nullopt;
    VoiceSlot& slot = *current.voices[index];
    std::lock_guard<std::mutex> voice_lock(slot.mutex);
    if (!slot.voice) return std::nullopt;
    return slot.voice->trace.produced;
}

}  // namespace g6shim::acphal
