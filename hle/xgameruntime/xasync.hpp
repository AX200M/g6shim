// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

namespace g6shim::xgameruntime::xasync {

inline constexpr Guid kProvider = guid(0x073b7dcb, 0x1fcf, 0x4030, 0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34,
                                       0x28);

inline constexpr std::uint32_t kWork = 0;
inline constexpr std::uint32_t kCompletion = 1;
inline constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;

inline constexpr std::uint32_t kOpBegin = 0;
inline constexpr std::uint32_t kOpDoWork = 1;
inline constexpr std::uint32_t kOpGetResult = 2;
inline constexpr std::uint32_t kOpCancel = 3;
inline constexpr std::uint32_t kOpCleanup = 4;

struct AsyncBlock {
    void* queue;
    void* context;
    void (*callback)(AsyncBlock*);
    std::size_t internal[4];
};

struct ProviderData {
    AsyncBlock* block;
    std::size_t buffer_size;
    void* buffer;
    void* context;
};

using Callback = void (*)(void*, bool);
using MonitorCallback = void (*)(void*, const void*, std::uint32_t);
using TerminatedCallback = void (*)(void*);
using CompletionRoutine = void (*)(AsyncBlock*);
using Provider = HResult (*)(std::uint32_t operation, const ProviderData* data);
using Work = HResult (*)(AsyncBlock* block);

extern const Object kObject;

HResult XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, void** out);
HResult XTaskQueueGetPort(const void* handle, std::uint32_t port, void** out);
HResult XTaskQueueCreateComposite(const void* work, const void* completion, void** out);
HResult XTaskQueueDuplicateHandle(const void* handle, void** out);
void XTaskQueueCloseHandle(const void* handle);
bool XTaskQueueDispatch(const void* handle, std::uint32_t port, std::uint32_t timeout_ms);
HResult XTaskQueueSubmitDelayedCallback(const void* handle, std::uint32_t port, std::uint32_t delay_ms,
                                        void* context, Callback callback);
HResult XTaskQueueTerminate(const void* handle, bool wait, void* context, TerminatedCallback callback);
HResult XTaskQueueRegisterMonitor(const void* handle, void* context, MonitorCallback callback,
                                  std::uint64_t* token);
void XTaskQueueUnregisterMonitor(const void* handle, std::uint64_t token);
bool XTaskQueueGetCurrentProcessTaskQueue(void** out);
void XTaskQueueSetCurrentProcessTaskQueue(const void* handle);

HResult XAsyncBegin(AsyncBlock* block, void* context, const void* identity, const char* identity_name,
                    Provider provider);
HResult XAsyncSchedule(AsyncBlock* block, std::uint32_t delay_ms);
void XAsyncComplete(AsyncBlock* block, HResult result, std::size_t required_size);
HResult XAsyncGetStatus(AsyncBlock* block, bool wait);
HResult XAsyncGetResultSize(AsyncBlock* block, std::size_t* out);
void XAsyncCancel(AsyncBlock* block);
HResult XAsyncGetResult(AsyncBlock* block, const void* identity, std::size_t buffer_size, void* buffer,
                        std::size_t* used);
HResult XAsyncRun(AsyncBlock* block, Work work);

HResult finished(AsyncBlock* block, const void* identity, HResult result);
HResult finished_with(AsyncBlock* block, const void* identity, HResult status, std::vector<std::uint8_t> bytes);

struct PendingResult {
    HResult status;
    std::vector<std::uint8_t> bytes;
};

using PendingWork = std::function<PendingResult()>;

HResult pending_with(AsyncBlock* block, const void* identity, PendingWork work);

template <class T>
HResult result(AsyncBlock* block, const void* identity, T* out) {
    return XAsyncGetResult(block, identity, sizeof(T), out, nullptr);
}

template <class T>
std::vector<std::uint8_t> bytes(const T& value) {
    std::vector<std::uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

std::span<const Interface> interfaces();

}  // namespace g6shim::xgameruntime::xasync
