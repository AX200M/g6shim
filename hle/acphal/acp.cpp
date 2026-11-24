// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acp.hpp"

#include "apu_memory.hpp"
#include "xma_voices.hpp"

#include <g6shim/log.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <optional>

namespace g6shim::acphal {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kLoadFlowgraph = 1;
constexpr std::uint32_t kRegisterMessages = 2;
constexpr std::uint32_t kUnregisterMessages = 3;

constexpr std::uint32_t kEnableXma = 5;
constexpr std::uint32_t kEnableXmaSet = 6;
constexpr std::uint32_t kDisableXma = 7;
constexpr std::uint32_t kDisableXmaSet = 8;
constexpr std::uint32_t kAdvanceXmaRead = 18;

constexpr std::uint32_t kFlowgraphCompleted = 0x02;
constexpr std::uint32_t kCommandCompleted = 0x10;
constexpr std::uint32_t kDisconnected = 0x80;

constexpr std::uint64_t kFramesPerSecond = 375;
constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::uint32_t kLoggedCommands = 64;

void put_le32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xff);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

void put_le64(std::uint8_t* out, std::uint64_t value) {
    put_le32(out, static_cast<std::uint32_t>(value & 0xffffffffu));
    put_le32(out + 4, static_cast<std::uint32_t>(value >> 32));
}

struct State {
    bool connected = false;
    std::uint32_t mask = 0;
    std::deque<Message> messages;
    std::size_t capacity = 0;
    std::uint32_t dropped = 0;
    Clock::time_point since = Clock::now();

    std::uint64_t usec() const {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since).count());
    }

    std::uint32_t frame() const {
        return static_cast<std::uint32_t>(usec() * kFramesPerSecond / kMicrosecondsPerSecond);
    }

    void post(std::uint32_t kind, const std::uint8_t* data, std::size_t length) {
        if ((mask & kind) == 0) return;
        if (messages.size() >= capacity) {
            dropped = dropped == UINT32_MAX ? dropped : dropped + 1;
            G6SHIM_TRACE("coprocessor message %lu dropped: %lu lost", kind, dropped);
            return;
        }
        Message message{};
        message.kind = kind;
        message.dropped = dropped;
        dropped = 0;
        message.usec = static_cast<std::uint32_t>(usec());
        if (data) std::memcpy(message.data, data, length > sizeof(message.data) ? sizeof(message.data) : length);
        messages.push_back(message);
    }
};

struct AcpObject {
    Acp acp;
    std::mutex mutex;
    State state;
};

static_assert(offsetof(AcpObject, acp) == 0);

AcpObject& object_of(Acp* self) { return *reinterpret_cast<AcpObject*>(self); }

std::mutex& state_mutex(Acp* self) { return object_of(self).mutex; }

HRESULT WINAPI acp_connect(Acp* self, std::uint32_t commands, std::uint32_t messages) {
    if (commands == 0) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(state_mutex(self));
    State& state = object_of(self).state;
    state.connected = true;
    state.capacity = messages;
    return S_OK;
}

HRESULT WINAPI acp_disconnect(Acp* self) {
    std::lock_guard<std::mutex> lock(state_mutex(self));
    State& state = object_of(self).state;
    if (!state.connected) return E_FAIL;
    state.post(kDisconnected, nullptr, 0);
    state.connected = false;
    return S_OK;
}

struct CommandWords {
    const void* data = nullptr;

    std::optional<std::uint32_t> operator()(std::size_t at) const {
        if (!data) return std::nullopt;
        std::uint32_t value = 0;
        std::memcpy(&value, static_cast<const std::uint8_t*>(data) + at * sizeof(std::uint32_t), sizeof(value));
        return value;
    }
};

bool xma_command(std::uint32_t command, const CommandWords& word) {
    switch (command) {
        case kEnableXma: xma_enable(word(0).value_or(UINT32_MAX)); return true;
        case kDisableXma: xma_disable(word(0).value_or(UINT32_MAX)); return true;
        case kEnableXmaSet:
        case kDisableXmaSet: {
            const std::uint32_t voices = xma_voice_count();
            for (std::uint32_t index = 0; index < voices; ++index) {
                if ((word(index / 32).value_or(0) & (1u << (index % 32))) == 0) continue;
                if (command == kEnableXmaSet)
                    xma_enable(index);
                else
                    xma_disable(index);
            }
            return true;
        }
        case kAdvanceXmaRead:
            xma_advance_read(word(0).value_or(UINT32_MAX), word(1).value_or(0));
            return true;
        default:
            return false;
    }
}

HRESULT WINAPI acp_submit(Acp* self, std::uint32_t command, std::uint64_t id, std::uint32_t frame, const void* data,
                          std::uint32_t notification) {
    static std::mutex seen_mutex;
    static std::uint64_t seen = 0;
    if (command < kLoggedCommands) {
        const std::uint64_t bit = std::uint64_t{1} << command;
        std::lock_guard<std::mutex> lock(seen_mutex);
        if ((seen & bit) == 0) {
            seen |= bit;
            G6SHIM_INFO("first coprocessor command of kind %lu", command);
        }
    }
    (void)frame;
    const CommandWords word{data};
    std::lock_guard<std::mutex> lock(state_mutex(self));
    State& state = object_of(self).state;
    switch (command) {
        case kRegisterMessages: state.mask |= word(0).value_or(0); break;
        case kUnregisterMessages: state.mask &= ~word(0).value_or(0); break;
        case kLoadFlowgraph: {
            std::uint8_t bytes[4];
            put_le32(bytes, word(1).value_or(0));
            state.post(kFlowgraphCompleted, bytes, sizeof(bytes));
            break;
        }
        default:
            xma_command(command, word);
            break;
    }
    if (id != 0) {
        std::uint8_t done[16] = {};
        put_le32(done, command);
        put_le64(done + 4, id);
        put_le32(done + 12, state.frame());
        state.post(kCommandCompleted, done, sizeof(done));
    }
    if (notification != 0) {
        auto* value = static_cast<std::uint8_t*>(
            apu_memory().virtual_address(notification, static_cast<std::uint32_t>(sizeof(std::uint32_t))));
        if (value) *reinterpret_cast<volatile std::uint32_t*>(value) = 1;
    }
    return S_OK;
}

bool WINAPI acp_pop_message(Acp* self, Message* out) {
    std::lock_guard<std::mutex> lock(state_mutex(self));
    State& state = object_of(self).state;
    if (state.messages.empty()) return false;
    const Message message = state.messages.front();
    state.messages.pop_front();
    if (out) std::memcpy(out, &message, sizeof(message));
    return true;
}

std::uint32_t WINAPI acp_message_count(Acp* self) {
    std::lock_guard<std::mutex> lock(state_mutex(self));
    return static_cast<std::uint32_t>(object_of(self).state.messages.size());
}

void WINAPI acp_release(Acp* self) { delete reinterpret_cast<AcpObject*>(self); }

const AcpMethods kAcpMethods = {acp_connect, acp_disconnect, acp_submit,
                                acp_pop_message, acp_message_count, acp_release};

}  // namespace

HRESULT create_acp(Acp** out) {
    if (!out) return E_INVALIDARG;
    auto* object = new (std::nothrow) AcpObject();
    if (!object) return E_OUTOFMEMORY;
    object->acp.methods = &kAcpMethods;
    *out = &object->acp;
    return S_OK;
}

}  // namespace g6shim::acphal
