// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace g6shim::xgameruntime::xasync {

namespace {

enum class Mode { kManual, kThreadPool, kSerialized, kImmediate };

std::optional<Mode> mode_from_raw(std::uint32_t mode) {
    switch (mode) {
        case 0: return Mode::kManual;
        case 1: return Mode::kThreadPool;
        case 2: return Mode::kSerialized;
        case 3: return Mode::kImmediate;
        default: return std::nullopt;
    }
}

struct Task {
    void* context;
    Callback callback;

    void run(bool canceled) const { callback(context, canceled); }
};

struct Port : std::enable_shared_from_this<Port> {
    explicit Port(Mode mode_) : mode(mode_) {}

    void submit(const Task& task);

    template <class Ended>
    std::optional<Task> take(std::uint32_t timeout_ms, Ended ended) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto waiting = [&] { return !tasks.empty() || ended(); };
        if (timeout_ms == kInfinite) {
            ready.wait(lock, waiting);
        } else {
            ready.wait_for(lock, std::chrono::milliseconds(timeout_ms), waiting);
        }
        if (tasks.empty()) return std::nullopt;
        const Task task = tasks.front();
        tasks.pop_front();
        return task;
    }

    Mode mode;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Task> tasks;
};

struct Pool {
    void submit(const Task& task) {
        std::unique_lock<std::mutex> lock(mutex);
        tasks.push_back(task);
        if (tasks.size() > idle) {
            lock.unlock();
            std::thread([this] { work(); }).detach();
        } else {
            ready.notify_one();
        }
    }

    void work() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            while (tasks.empty()) {
                ++idle;
                ready.wait(lock);
                --idle;
            }
            const Task task = tasks.front();
            tasks.pop_front();
            lock.unlock();
            task.run(false);
            lock.lock();
        }
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Task> tasks;
    std::size_t idle = 0;
};

Pool& pool() {
    static Pool* instance = new Pool();
    return *instance;
}

std::shared_ptr<Port> make_port(Mode mode) {
    auto port = std::make_shared<Port>(mode);
    if (mode == Mode::kSerialized) {
        std::shared_ptr<Port> worker = port;
        std::thread([worker] {
            for (;;) {
                const std::optional<Task> task = worker->take(1000, [] { return false; });
                if (task) {
                    task->run(false);
                } else if (worker.use_count() == 1) {
                    break;
                }
            }
        }).detach();
    }
    return port;
}

void Port::submit(const Task& task) {
    switch (mode) {
        case Mode::kImmediate:
            task.run(false);
            break;
        case Mode::kThreadPool:
            pool().submit(task);
            break;
        case Mode::kManual:
        case Mode::kSerialized: {
            {
                std::lock_guard<std::mutex> lock(mutex);
                tasks.push_back(task);
            }
            ready.notify_one();
            break;
        }
    }
}

struct Monitor {
    std::uint64_t id;
    void* context;
    MonitorCallback callback;
};

struct Queue {
    Queue(std::shared_ptr<Port> work, std::shared_ptr<Port> completion)
        : ports{std::move(work), std::move(completion)} {}

    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }

    HResult submit(std::uint32_t port, std::uint32_t delay_ms, Task task);
    void deliver(std::uint32_t port, const Task& task);

    std::array<std::shared_ptr<Port>, 2> ports;
    std::mutex monitors_mutex;
    std::vector<Monitor> monitors;
    std::atomic<bool> terminated{false};
    std::atomic<std::size_t> refs{1};
};

class QueueRef {
public:
    QueueRef() = default;
    explicit QueueRef(Queue* queue) : queue_(queue) {}
    ~QueueRef() { reset(); }
    QueueRef(const QueueRef&) = delete;
    QueueRef& operator=(const QueueRef&) = delete;
    QueueRef(QueueRef&& other) noexcept : queue_(other.queue_) { other.queue_ = nullptr; }
    QueueRef& operator=(QueueRef&& other) noexcept {
        if (this != &other) {
            reset();
            queue_ = other.queue_;
            other.queue_ = nullptr;
        }
        return *this;
    }

    Queue* get() const { return queue_; }
    explicit operator bool() const { return queue_ != nullptr; }
    Queue* detach() {
        Queue* queue = queue_;
        queue_ = nullptr;
        return queue;
    }
    void reset() {
        if (queue_ != nullptr) {
            queue_->release();
            queue_ = nullptr;
        }
    }

private:
    Queue* queue_ = nullptr;
};

HResult Queue::submit(std::uint32_t port, std::uint32_t delay_ms, Task task) {
    if (port > kCompletion) return kInvalidArg;
    if (terminated.load(std::memory_order_acquire)) return kAbort;
    if (delay_ms == 0) {
        deliver(port, task);
    } else {
        retain();
        std::thread([this, port, delay_ms, task] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            deliver(port, task);
            release();
        }).detach();
    }
    return kOk;
}

void Queue::deliver(std::uint32_t port, const Task& task) {
    ports[port]->submit(task);
    std::vector<Monitor> copy;
    {
        std::lock_guard<std::mutex> lock(monitors_mutex);
        copy = monitors;
    }
    for (const Monitor& monitor : copy) monitor.callback(monitor.context, this, port);
}

bool g_process_initialized = false;
Queue* g_process_queue = nullptr;

std::mutex& process_mutex() {
    static std::mutex* instance = new std::mutex();
    return *instance;
}

std::optional<QueueRef> process_queue() {
    std::lock_guard<std::mutex> lock(process_mutex());
    if (g_process_initialized) {
        if (g_process_queue == nullptr) return std::nullopt;
        g_process_queue->retain();
        return QueueRef(g_process_queue);
    }
    Queue* queue = new Queue(make_port(Mode::kThreadPool), make_port(Mode::kThreadPool));
    g_process_queue = queue;
    g_process_initialized = true;
    queue->retain();
    return QueueRef(queue);
}

std::optional<QueueRef> queue_ref(const void* handle) {
    if (handle == nullptr) return process_queue();
    Queue* queue = static_cast<Queue*>(const_cast<void*>(handle));
    queue->retain();
    return QueueRef(queue);
}

struct Call {
    Provider provider = nullptr;
    void* context = nullptr;
    const void* identity = nullptr;
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;

    HResult invoke(std::uint32_t operation, AsyncBlock* block, std::size_t size, void* buffer) const {
        const ProviderData data{block, size, buffer, context};
        return provider(operation, &data);
    }
};

std::mutex& calls_mutex() {
    static std::mutex* instance = new std::mutex();
    return *instance;
}

std::unordered_map<AsyncBlock*, std::shared_ptr<Call>>& calls() {
    static auto* instance = new std::unordered_map<AsyncBlock*, std::shared_ptr<Call>>();
    return *instance;
}

std::shared_ptr<Call> find_call(AsyncBlock* block) {
    std::lock_guard<std::mutex> lock(calls_mutex());
    const auto found = calls().find(block);
    return found == calls().end() ? nullptr : found->second;
}

HResult status(AsyncBlock* block) {
    std::atomic_ref<std::size_t> word(block->internal[1]);
    return static_cast<HResult>(static_cast<std::uint32_t>(word.load(std::memory_order_acquire)));
}

std::size_t result_size(AsyncBlock* block) {
    std::atomic_ref<std::size_t> word(block->internal[2]);
    return word.load(std::memory_order_acquire);
}

void store_status(AsyncBlock* block, HResult result) {
    std::atomic_ref<std::size_t> word(block->internal[1]);
    word.store(static_cast<std::size_t>(static_cast<std::uint32_t>(result)), std::memory_order_release);
}

void store_result_size(AsyncBlock* block, std::size_t size) {
    std::atomic_ref<std::size_t> word(block->internal[2]);
    word.store(size, std::memory_order_release);
}

void release_call(AsyncBlock* block) {
    std::shared_ptr<Call> call;
    {
        std::lock_guard<std::mutex> lock(calls_mutex());
        const auto found = calls().find(block);
        if (found != calls().end()) {
            call = std::move(found->second);
            calls().erase(found);
        }
    }
    if (call) call->invoke(kOpCleanup, block, 0, nullptr);
}

void scheduled_work(void* context, bool canceled) {
    auto* block = static_cast<AsyncBlock*>(context);
    const std::shared_ptr<Call> call = find_call(block);
    if (!call) return;
    const HResult result = canceled ? kAbort : call->invoke(kOpDoWork, block, 0, nullptr);
    if (result != kPending) XAsyncComplete(block, result, 0);
}

void completed_work(void* context, bool) {
    auto* block = static_cast<AsyncBlock*>(context);
    const CompletionRoutine routine = block->callback;
    if (routine != nullptr) routine(block);
}

struct ImmediateResult {
    HResult status;
    std::vector<std::uint8_t> bytes;
};

HResult immediate_provider(std::uint32_t operation, const ProviderData* data) {
    auto* result = static_cast<ImmediateResult*>(data->context);
    switch (operation) {
        case kOpCleanup:
            delete result;
            break;
        case kOpBegin: {
            const std::size_t length = result->status >= 0 ? result->bytes.size() : 0;
            XAsyncComplete(data->block, result->status, length);
            break;
        }
        case kOpGetResult:
            if (result->status >= 0 && data->buffer != nullptr && !result->bytes.empty())
                std::memcpy(data->buffer, result->bytes.data(), result->bytes.size());
            break;
        default:
            break;
    }
    return kOk;
}

struct PendingState {
    std::mutex mutex;
    PendingWork work;
    std::vector<std::uint8_t> bytes;
};

HResult pending_provider(std::uint32_t operation, const ProviderData* data) {
    auto* state = static_cast<PendingState*>(data->context);
    switch (operation) {
        case kOpCleanup:
            delete state;
            break;
        case kOpBegin: {
            PendingWork work;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                work = std::move(state->work);
            }
            AsyncBlock* block = data->block;
            std::thread([state, block, work = std::move(work)] {
                const PendingResult result = work ? work() : PendingResult{kAbort, {}};
                const std::size_t length = result.status >= 0 ? result.bytes.size() : 0;
                if (result.status >= 0) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->bytes = result.bytes;
                }
                XAsyncComplete(block, result.status, length);
            }).detach();
            break;
        }
        case kOpGetResult: {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->bytes.empty() && data->buffer != nullptr)
                std::memcpy(data->buffer, state->bytes.data(), state->bytes.size());
            break;
        }
        default:
            break;
    }
    return kOk;
}

struct WorkHolder {
    Work work;
};

HResult run_provider(std::uint32_t operation, const ProviderData* data) {
    switch (operation) {
        case kOpCleanup:
            delete static_cast<WorkHolder*>(data->context);
            return kOk;
        case kOpBegin:
            return XAsyncSchedule(data->block, 0);
        case kOpDoWork: {
            auto* holder = static_cast<WorkHolder*>(data->context);
            return holder->work(data->block);
        }
        default:
            return kOk;
    }
}

using This = Object*;

HResult m_get_status(This, AsyncBlock* block, std::uint8_t wait) { return XAsyncGetStatus(block, wait != 0); }
HResult m_get_result_size(This, AsyncBlock* block, std::size_t* out) { return XAsyncGetResultSize(block, out); }
void m_cancel(This, AsyncBlock* block) { XAsyncCancel(block); }
HResult m_run(This, AsyncBlock* block, Work work) { return XAsyncRun(block, work); }
HResult m_begin(This, AsyncBlock* block, void* context, const void* identity, const char* name, Provider provider) {
    return XAsyncBegin(block, context, identity, name, provider);
}
HResult m_schedule(This, AsyncBlock* block, std::uint32_t delay) { return XAsyncSchedule(block, delay); }
void m_complete(This, AsyncBlock* block, HResult result, std::size_t size) { XAsyncComplete(block, result, size); }
HResult m_get_result(This, AsyncBlock* block, const void* identity, std::size_t size, void* buffer,
                     std::size_t* used) {
    return XAsyncGetResult(block, identity, size, buffer, used);
}
HResult m_create(This, std::uint32_t work, std::uint32_t completion, void** out) {
    return XTaskQueueCreate(work, completion, out);
}
HResult m_create_composite(This, const void* work, const void* completion, void** out) {
    return XTaskQueueCreateComposite(work, completion, out);
}
HResult m_get_port(This, const void* handle, std::uint32_t port, void** out) {
    return XTaskQueueGetPort(handle, port, out);
}
HResult m_duplicate(This, const void* handle, void** out) { return XTaskQueueDuplicateHandle(handle, out); }
bool m_dispatch(This, const void* handle, std::uint32_t port, std::uint32_t timeout) {
    return XTaskQueueDispatch(handle, port, timeout);
}
void m_close(This, const void* handle) { XTaskQueueCloseHandle(handle); }
HResult m_submit(This, const void* handle, std::uint32_t port, void* context, Callback callback) {
    return XTaskQueueSubmitDelayedCallback(handle, port, 0, context, callback);
}
HResult m_submit_delayed(This, const void* handle, std::uint32_t port, std::uint32_t delay, void* context,
                          Callback callback) {
    return XTaskQueueSubmitDelayedCallback(handle, port, delay, context, callback);
}
HResult m_terminate(This, const void* handle, std::uint8_t wait, void* context, TerminatedCallback callback) {
    return XTaskQueueTerminate(handle, wait != 0, context, callback);
}
HResult m_register_monitor(This, const void* handle, void* context, MonitorCallback callback,
                           std::uint64_t* token) {
    return XTaskQueueRegisterMonitor(handle, context, callback, token);
}
void m_unregister_monitor(This, const void* handle, std::uint64_t token) {
    XTaskQueueUnregisterMonitor(handle, token);
}
bool m_get_process_queue(This, void** out) { return XTaskQueueGetCurrentProcessTaskQueue(out); }
void m_set_process_queue(This, const void* handle) { XTaskQueueSetCurrentProcessTaskQueue(handle); }
void m_check_thread(This) {}

const std::array<const void*, 31> kVtable = {
    slot(&missing<0>),  slot(&add_ref),           slot(&add_ref),           slot(&m_get_status),
    slot(&m_get_result_size), slot(&m_cancel),     slot(&m_run),             slot(&m_begin),
    slot(&missing<8>),  slot(&m_schedule),        slot(&m_complete),        slot(&m_get_result),
    slot(&m_create),    slot(&m_create_composite), slot(&m_get_port),       slot(&m_duplicate),
    slot(&m_dispatch),  slot(&m_close),           slot(&m_submit),          slot(&m_submit_delayed),
    slot(&missing<20>), slot(&missing<21>),       slot(&m_terminate),       slot(&m_register_monitor),
    slot(&m_unregister_monitor), slot(&m_get_process_queue), slot(&m_set_process_queue), slot(&missing<27>),
    slot(&missing<28>), slot(&m_check_thread),    slot(&missing<30>)};

}  // namespace

const Object kObject{kVtable.data(), "task queue"};

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kProvider, &kObject}};
    return entries;
}

HResult XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, void** out) {
    const std::optional<Mode> work_mode = mode_from_raw(work);
    const std::optional<Mode> completion_mode = mode_from_raw(completion);
    if (!work_mode || !completion_mode || out == nullptr) return kInvalidArg;
    Queue* queue = new Queue(make_port(*work_mode), make_port(*completion_mode));
    *out = queue;
    return kOk;
}

HResult XTaskQueueGetPort(const void* handle, std::uint32_t port, void** out) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue || port > kCompletion || out == nullptr) return kInvalidArg;
    *out = queue->get()->ports[port].get();
    return kOk;
}

HResult XTaskQueueCreateComposite(const void* work, const void* completion, void** out) {
    if (work == nullptr || completion == nullptr || out == nullptr) return kInvalidArg;
    const std::shared_ptr<Port> work_port = static_cast<Port*>(const_cast<void*>(work))->weak_from_this().lock();
    const std::shared_ptr<Port> completion_port =
        static_cast<Port*>(const_cast<void*>(completion))->weak_from_this().lock();
    if (!work_port || !completion_port) return kInvalidArg;
    *out = new Queue(work_port, completion_port);
    return kOk;
}

HResult XTaskQueueDuplicateHandle(const void* handle, void** out) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue || out == nullptr) return kInvalidArg;
    queue->get()->retain();
    *out = queue->get();
    return kOk;
}

void XTaskQueueCloseHandle(const void* handle) {
    if (handle != nullptr) static_cast<Queue*>(const_cast<void*>(handle))->release();
}

bool XTaskQueueDispatch(const void* handle, std::uint32_t port, std::uint32_t timeout_ms) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue || port > kCompletion) return false;
    const std::shared_ptr<Port>& target = queue->get()->ports[port];
    if (target->mode != Mode::kManual) return false;
    const std::optional<Task> task =
        target->take(timeout_ms, [&queue] { return queue->get()->terminated.load(std::memory_order_acquire); });
    if (!task) return false;
    task->run(false);
    return true;
}

HResult XTaskQueueSubmitDelayedCallback(const void* handle, std::uint32_t port, std::uint32_t delay_ms,
                                        void* context, Callback callback) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue || callback == nullptr) return kInvalidArg;
    return queue->get()->submit(port, delay_ms, Task{context, callback});
}

HResult XTaskQueueTerminate(const void* handle, bool, void* context, TerminatedCallback callback) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue) return kInvalidArg;
    queue->get()->terminated.store(true, std::memory_order_release);
    for (const std::shared_ptr<Port>& port : queue->get()->ports) {
        std::vector<Task> pending;
        {
            std::lock_guard<std::mutex> lock(port->mutex);
            pending.assign(port->tasks.begin(), port->tasks.end());
            port->tasks.clear();
        }
        port->ready.notify_all();
        for (const Task& task : pending) task.run(true);
    }
    if (callback != nullptr) callback(context);
    return kOk;
}

HResult XTaskQueueRegisterMonitor(const void* handle, void* context, MonitorCallback callback,
                                  std::uint64_t* token_out) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue || callback == nullptr || token_out == nullptr) return kInvalidArg;
    static std::atomic<std::uint64_t> next{1};
    const std::uint64_t id = next.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queue->get()->monitors_mutex);
        queue->get()->monitors.push_back(Monitor{id, context, callback});
    }
    *token_out = id;
    return kOk;
}

void XTaskQueueUnregisterMonitor(const void* handle, std::uint64_t token) {
    const std::optional<QueueRef> queue = queue_ref(handle);
    if (!queue) return;
    std::lock_guard<std::mutex> lock(queue->get()->monitors_mutex);
    std::vector<Monitor>& monitors = queue->get()->monitors;
    monitors.erase(std::remove_if(monitors.begin(), monitors.end(), [token](const Monitor& monitor) {
                       return monitor.id == token;
                   }),
                   monitors.end());
}

bool XTaskQueueGetCurrentProcessTaskQueue(void** out) {
    const std::optional<QueueRef> found = process_queue();
    if (out != nullptr) {
        if (found) {
            found->get()->retain();
            *out = found->get();
        } else {
            *out = nullptr;
        }
    }
    return found.has_value();
}

void XTaskQueueSetCurrentProcessTaskQueue(const void* handle) {
    std::optional<QueueRef> next;
    if (handle != nullptr) next = queue_ref(handle);
    std::lock_guard<std::mutex> lock(process_mutex());
    if (g_process_queue != nullptr) g_process_queue->release();
    g_process_queue = next ? next->detach() : nullptr;
    g_process_initialized = true;
}

HResult XAsyncBegin(AsyncBlock* block, void* context, const void* identity, const char*, Provider provider) {
    if (provider == nullptr) return kInvalidArg;
    if (block == nullptr || find_call(block) != nullptr) return kInvalidArg;
    store_status(block, kPending);
    store_result_size(block, 0);
    auto call = std::make_shared<Call>();
    call->provider = provider;
    call->context = context;
    call->identity = identity;
    {
        std::lock_guard<std::mutex> lock(calls_mutex());
        calls()[block] = call;
    }
    const HResult result = call->invoke(kOpBegin, block, 0, nullptr);
    if (result < 0) XAsyncComplete(block, result, 0);
    return kOk;
}

HResult XAsyncSchedule(AsyncBlock* block, std::uint32_t delay_ms) {
    if (block == nullptr || find_call(block) == nullptr) return kInvalidArg;
    const std::optional<QueueRef> queue = queue_ref(block->queue);
    if (!queue) return kInvalidArg;
    return queue->get()->submit(kWork, delay_ms, Task{block, &scheduled_work});
}

void XAsyncComplete(AsyncBlock* block, HResult result, std::size_t required_size) {
    const std::shared_ptr<Call> call = find_call(block);
    if (!call) return;
    if (result == kPending) return;
    std::unique_lock<std::mutex> lock(call->mutex);
    if (call->done) return;
    void* const handle = block->queue;
    const CompletionRoutine routine = block->callback;
    if (result < 0 || required_size == 0) release_call(block);
    store_result_size(block, required_size);
    store_status(block, result);
    call->done = true;
    call->finished.notify_all();
    lock.unlock();
    if (routine != nullptr) {
        const Task task{block, &completed_work};
        const std::optional<QueueRef> queue = queue_ref(handle);
        if (!queue || queue->get()->submit(kCompletion, 0, task) != kOk) task.run(true);
    }
}

HResult XAsyncGetStatus(AsyncBlock* block, bool wait) {
    if (block == nullptr) return kInvalidArg;
    if (wait) {
        if (const std::shared_ptr<Call> call = find_call(block)) {
            std::unique_lock<std::mutex> lock(call->mutex);
            call->finished.wait(lock, [&call] { return call->done; });
        }
    }
    return status(block);
}

HResult XAsyncGetResultSize(AsyncBlock* block, std::size_t* out) {
    if (block == nullptr || out == nullptr) return kInvalidArg;
    const HResult result = status(block);
    if (result >= 0) *out = result_size(block);
    return result;
}

void XAsyncCancel(AsyncBlock* block) {
    const std::shared_ptr<Call> call = find_call(block);
    if (!call) return;
    call->invoke(kOpCancel, block, 0, nullptr);
    XAsyncComplete(block, kAbort, 0);
}

HResult XAsyncGetResult(AsyncBlock* block, const void* identity, std::size_t buffer_size, void* buffer,
                        std::size_t* used) {
    if (block == nullptr) return kInvalidArg;
    const HResult result = status(block);
    const std::size_t size = result_size(block);
    if (result < 0 || size == 0) {
        if (result >= 0 && used != nullptr) *used = 0;
        return result;
    }
    const std::shared_ptr<Call> call = find_call(block);
    if (!call) return kIllegalMethodCall;
    if (call->identity != identity) return kInvalidArg;
    if (buffer_size < size || buffer == nullptr) return kNotSufficientBuffer;
    const HResult written = call->invoke(kOpGetResult, block, buffer_size, buffer);
    if (used != nullptr) *used = size;
    release_call(block);
    return written;
}

HResult XAsyncRun(AsyncBlock* block, Work work) {
    if (work == nullptr) return kInvalidArg;
    auto* holder = new WorkHolder{work};
    const HResult result = XAsyncBegin(block, holder, slot(&XAsyncRun), nullptr, &run_provider);
    if (result < 0) delete holder;
    return result;
}

HResult finished(AsyncBlock* block, const void* identity, HResult result) {
    return finished_with(block, identity, result, {});
}

HResult finished_with(AsyncBlock* block, const void* identity, HResult status_value,
                      std::vector<std::uint8_t> bytes_value) {
    auto* context = new ImmediateResult{status_value, std::move(bytes_value)};
    const HResult result = XAsyncBegin(block, context, identity, nullptr, &immediate_provider);
    if (result < 0) delete context;
    return result;
}

HResult pending_with(AsyncBlock* block, const void* identity, PendingWork work) {
    auto* state = new PendingState();
    state->work = std::move(work);
    const HResult result = XAsyncBegin(block, state, identity, nullptr, &pending_provider);
    if (result < 0) delete state;
    return result;
}

}  // namespace g6shim::xgameruntime::xasync

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xasync_interfaces() {
    return xasync::interfaces();
}
