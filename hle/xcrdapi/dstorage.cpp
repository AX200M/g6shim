// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dstorage.hpp"

#include "bcpack.hpp"

#include <g6shim/drives.hpp>
#include <g6shim/log.hpp>
#include <g6shim/shader/error.hpp>
#include <g6shim/shader/inflate.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <system_error>
#include <thread>
#include <tuple>

extern "C" void* WINAPI XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type, ULONGLONG flags,
                                         DWORD protect);
extern "C" void* WINAPI XMemMapPhysicalPages(void* address, SIZE_T count, const SIZE_T* pages);

namespace g6shim::xcrdapi {

std::string guid_text(const Guid* iid) {
    if (iid == nullptr) return "null";
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  iid->data1, iid->data2, iid->data3, iid->data4[0], iid->data4[1], iid->data4[2],
                  iid->data4[3], iid->data4[4], iid->data4[5], iid->data4[6], iid->data4[7]);
    return buffer;
}

namespace {

constexpr Guid kFactoryInterface{0x62959d11u, 0xd45f, 0x4884,
                                 {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr DWORD kConsole64KiBPageReserve = MEM_RESERVE | 0x2040'0000u;
constexpr std::size_t kFactorySlots = 16;
constexpr std::size_t kFileSlots = 8;
constexpr std::size_t kStatusSlots = 5;
constexpr std::size_t kQueueSlots = 12;

const void* const* file_methods();
const void* const* status_methods();
const void* const* queue_methods();
const void* const* factory_methods();

struct File {
    explicit File(HANDLE value) : handle(value) {}
    ~File();
    HANDLE handle;
};

struct Queue {
    explicit Queue(std::uint64_t value) : id(value) {}
    ~Queue();
    std::uint64_t id;
};

File::~File() {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}

Queue::~Queue() { close_lane(id); }

template <typename F>
const void* as_method(F function) {
    return reinterpret_cast<const void*>(function);
}

std::string narrow(const char* text) { return text == nullptr ? std::string{} : std::string{text}; }

std::string narrow(const wchar_t* text) {
    if (text == nullptr) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::vector<char> buffer(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer.data(), needed, nullptr, nullptr);
    return std::string(buffer.data());
}

HRESULT from_win32(DWORD error) {
    return static_cast<HRESULT>(0x8007'0000u | (error & 0xffffu));
}

std::atomic<std::uint64_t>& reading_time() {
    static std::atomic<std::uint64_t> time{0};
    return time;
}

constexpr wchar_t kTablesVariable[] = L"G6SHIM_BCPACK_TABLES";
constexpr wchar_t kTablesFileName[] = L"bcpack-tables.bin";
constexpr std::uint64_t kMaxTableFileBytes = 4u << 20;

std::optional<std::wstring> tables_variable_path() {
    const DWORD needed = GetEnvironmentVariableW(kTablesVariable, nullptr, 0);
    if (needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(kTablesVariable, value.data(), needed);
    if (written == 0 || written >= needed) return std::nullopt;
    value.resize(written);
    if (value.empty()) return std::nullopt;
    return value;
}

std::wstring module_folder() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_folder), &module))
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
        if (path.size() > 0x8000) return {};
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    return path;
}

bool read_table_file(const std::wstring& path, std::vector<std::uint8_t>& bytes,
                     std::string& reason) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        reason = std::string("cannot be opened (error ") + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaxTableFileBytes) {
        reason = "cannot be read";
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const DWORD want =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 0xffff'ffffu));
        DWORD got = 0;
        if (!ReadFile(file, bytes.data() + done, want, &got, nullptr) || got == 0) {
            reason = std::string("cannot be read (error ") + std::to_string(GetLastError()) + ")";
            CloseHandle(file);
            return false;
        }
        done += got;
    }
    CloseHandle(file);
    return true;
}

std::optional<Tables> load_tables(std::string& reason) {
    std::wstring location;
    if (const auto from_variable = tables_variable_path()) {
        location = *from_variable;
    } else if (const std::wstring folder = module_folder(); !folder.empty()) {
        location = folder + kTablesFileName;
    }
    if (location.empty()) {
        reason = "no table file location";
        return std::nullopt;
    }
    const std::string shown = narrow(location.c_str());
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!read_table_file(location, bytes, why)) {
        reason = shown + " " + why;
        return std::nullopt;
    }
    BcpackError error{};
    auto parsed = Tables::parse(bytes, &error);
    if (!parsed) {
        reason = shown + ": " + (error.reason != nullptr ? error.reason : "unknown");
        return std::nullopt;
    }
    return parsed;
}

HRESULT read_into(const Request& request, std::span<std::uint8_t> out) {
    if ((request.options & kOptionMemorySource) != 0) {
        if (!out.empty() && request.source != nullptr)
            std::memcpy(out.data(), request.source, out.size());
        return kSucceeded;
    }
    const auto* instance = static_cast<const Instance<File>*>(request.source);
    if (instance == nullptr) return kInvalidArgument;
    const HANDLE handle = instance->state.handle;
    const auto started = std::chrono::steady_clock::now();
    std::size_t done = 0;
    HRESULT result = kSucceeded;
    while (done < out.size()) {
        OVERLAPPED overlapped{};
        const std::uint64_t at = request.offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xffff'ffffu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        const std::size_t left = out.size() - done;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(left, 0xffff'ffffu));
        DWORD got = 0;
        if (!ReadFile(handle, out.data() + done, want, &got, &overlapped)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                result = from_win32(error);
                break;
            }
            if (!GetOverlappedResult(handle, &overlapped, &got, TRUE)) {
                result = from_win32(GetLastError());
                break;
            }
        }
        if (got == 0) {
            result = kEndOfFile;
            break;
        }
        done += got;
    }
    const auto spent = std::chrono::steady_clock::now() - started;
    reading_time().fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count()),
        std::memory_order_relaxed);
    return result;
}

struct TablesCache {
    bool loaded = false;
    std::shared_ptr<const Tables> tables;
    std::string why;
};

std::mutex& tables_mutex() {
    static std::mutex mutex;
    return mutex;
}

TablesCache& tables_cache() {
    static TablesCache cache;
    return cache;
}

std::shared_ptr<const Tables> bcpack_tables() {
    static std::atomic<bool> warned{false};
    std::lock_guard<std::mutex> lock(tables_mutex());
    TablesCache& cache = tables_cache();
    if (!cache.loaded) {
        cache.loaded = true;
        std::string why;
        auto parsed = load_tables(why);
        if (parsed) {
            cache.tables = std::make_shared<const Tables>(std::move(*parsed));
        } else {
            cache.why = std::move(why);
        }
    }
    if (cache.tables) return cache.tables;
    if (!warned.exchange(true, std::memory_order_relaxed))
        G6SHIM_WARN("dstorage: BCPack requests fail: %s (more at debug)", cache.why.c_str());
    else
        G6SHIM_TRACE("dstorage: BCPack request failed: no tables");
    return nullptr;
}

HRESULT inflate_stream(std::span<const std::uint8_t> source, std::span<std::uint8_t> out,
                       std::size_t& written) {
    try {
        const std::vector<std::uint8_t> inflated = shader::inflate_zlib(source, out.size());
        if (inflated.size() > out.size()) return kInvalidData;
        if (!inflated.empty()) std::memcpy(out.data(), inflated.data(), inflated.size());
        written = inflated.size();
        return kSucceeded;
    } catch (const shader::ParseError& error) {
        G6SHIM_TRACE("dstorage: zlib stream: %s", error.what());
        return kInvalidData;
    } catch (const std::bad_alloc&) {
        return kOutOfMemory;
    }
}

HRESULT read_source(const Request& request, std::vector<std::uint8_t>& owned,
                    std::span<const std::uint8_t>& source) {
    const std::size_t size = request.source_size;
    if ((request.options & kOptionMemorySource) != 0) {
        source = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(request.source),
                                               size);
        return kSucceeded;
    }
    try {
        owned.assign(size, 0);
    } catch (const std::bad_alloc&) {
        return kOutOfMemory;
    }
    source = owned;
    return read_into(request, owned);
}

HRESULT into_destination(const Request& request, std::size_t len,
                         const std::function<HRESULT(std::span<std::uint8_t>)>& fill) {
    if ((request.options & kOptionPageDestination) == 0) {
        return fill(std::span<std::uint8_t>(static_cast<std::uint8_t*>(request.destination), len));
    }
    const std::size_t offset = static_cast<std::size_t>(request.page_offset & 0xffffu);
    const std::size_t count = (offset + len + kDestinationPageBytes - 1) / kDestinationPageBytes;
    if (count == 0) return fill(std::span<std::uint8_t>{});
    const auto* pages = static_cast<const std::size_t*>(request.destination);
    auto* scratch = static_cast<std::uint8_t*>(
        XMemVirtualAlloc(nullptr, count * kDestinationPageBytes, kConsole64KiBPageReserve, 0,
                         PAGE_READWRITE));
    if (scratch == nullptr) return kInvalidArgument;
    if (XMemMapPhysicalPages(scratch, count, pages) != scratch) {
        XMemMapPhysicalPages(scratch, count, nullptr);
        VirtualFree(scratch, 0, MEM_RELEASE);
        return kInvalidArgument;
    }
    const HRESULT result = fill(std::span<std::uint8_t>(scratch + offset, len));
    XMemMapPhysicalPages(scratch, count, nullptr);
    VirtualFree(scratch, 0, MEM_RELEASE);
    return result;
}

HRESULT execute(const Request& request) {
    const auto fail = [&request](HRESULT hr, const char* why) {
        G6SHIM_WARN(
            "dstorage: request failed: %s (options 0x%llx, offset %llu, source %u, destination %u, "
            "intermediate %u, tag %llu, name \"%s\", hr 0x%08lx)",
            why, static_cast<unsigned long long>(request.options),
            static_cast<unsigned long long>(request.offset), request.source_size,
            request.destination_size, request.intermediate_size,
            static_cast<unsigned long long>(request.tag), narrow(request.name).c_str(),
            static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
        return hr;
    };
    const std::uint64_t options = request.options;
    if ((options & ~(kOptionZlib | kOptionBcpack | kOptionPageDestination | kOptionMemorySource)) !=
        0)
        return fail(kNotImplemented, "unsupported options");
    if (request.source == nullptr || request.destination == nullptr)
        return fail(kInvalidArgument, "no source or destination");
    const std::uint8_t bc = static_cast<std::uint8_t>((options & kOptionBcpack) >> 1);
    const std::size_t size = request.destination_size;
    if ((options & (kOptionZlib | kOptionBcpack)) == 0) {
        if (request.source_size > request.destination_size)
            return fail(kInvalidArgument, "source larger than destination");
        const HRESULT copy = into_destination(
            request, request.source_size,
            [&request](std::span<std::uint8_t> out) { return read_into(request, out); });
        return copy == kSucceeded ? kSucceeded : fail(copy, "copying");
    }
    std::shared_ptr<const Tables> tables;
    if (bc != 0) {
        tables = bcpack_tables();
        if (tables == nullptr) return kNotImplemented;
    }
    std::vector<std::uint8_t> owned;
    std::span<const std::uint8_t> source;
    HRESULT result = read_source(request, owned, source);
    if (result != kSucceeded) return fail(result, "reading the source");
    std::vector<std::uint8_t> packed;
    if (bc != 0 && (options & kOptionZlib) != 0) {
        try {
            packed.assign(request.intermediate_size, 0);
        } catch (const std::bad_alloc&) {
            return fail(kOutOfMemory, "zlib");
        }
        std::size_t written = 0;
        result = inflate_stream(source, packed, written);
        if (result != kSucceeded) return fail(result, "zlib");
        packed.resize(written);
        source = packed;
    }
    result = into_destination(request, size, [&](std::span<std::uint8_t> out) -> HRESULT {
        if (tables != nullptr) {
            if (const auto error = decode(bc, source, out, *tables))
                return fail(kInvalidData, error->reason);
            return kSucceeded;
        }
        std::size_t written = 0;
        const HRESULT inflated = inflate_stream(source, out, written);
        return inflated == kSucceeded ? kSucceeded : fail(inflated, "zlib");
    });
    if (result != kSucceeded) return result;
    G6SHIM_TRACE("dstorage: request done (options 0x%llx, offset %llu, size %zu)",
                 static_cast<unsigned long long>(options),
                 static_cast<unsigned long long>(request.offset), size);
    return kSucceeded;
}

std::uint32_t WINAPI add_ref(const Object*) { return 1; }

HRESULT WINAPI open_file(const Object*, const wchar_t* path, const Guid* iid, void** out) {
    if (path == nullptr) return kInvalidArgument;
    const std::string name = narrow(path);
    const auto host = g6shim::drives::translate(path);
    const HANDLE handle = CreateFileW(host ? host->c_str() : path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        G6SHIM_WARN("dstorage: opening a file failed: %s: error %lu", name.c_str(), error);
        return from_win32(error);
    }
    G6SHIM_INFO("dstorage: file opened: %s (%s)", name.c_str(), guid_text(iid).c_str());
    try {
        return put(out, const_cast<void*>(
                            create_instance<File>(Object{file_methods(), "dstorage file"}, handle)));
    } catch (const std::bad_alloc&) {
        CloseHandle(handle);
        return kOutOfMemory;
    }
}

HRESULT WINAPI create_status_array(const Object*, std::uint32_t capacity, const char* name,
                                   const Guid* iid, void** out) {
    G6SHIM_INFO("dstorage: status array: capacity %u, name \"%s\", iid %s", capacity,
                narrow(name).c_str(), guid_text(iid).c_str());
    try {
        return put(out, const_cast<void*>(create_instance<StatusArray>(
                             Object{status_methods(), "dstorage status array"}, capacity)));
    } catch (const std::bad_alloc&) {
        return kOutOfMemory;
    }
}

HRESULT WINAPI create_queue(const Object*, const QueueDesc* desc, const Guid* iid, void** out) {
    if (desc == nullptr) return kInvalidArgument;
    G6SHIM_INFO("dstorage: queue: source %llu, capacity %u, priority %d, name \"%s\", unknown "
                "%llu, iid %s",
                static_cast<unsigned long long>(desc->source_type), desc->capacity,
                static_cast<int>(desc->priority), narrow(desc->name).c_str(),
                static_cast<unsigned long long>(desc->unknown), guid_text(iid).c_str());
    Scheduler* const state = scheduler();
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        id = state->lanes.next_id;
        state->lanes.next_id += 1;
        state->lanes.lanes.emplace(id, Lane::make(desc->priority, desc->capacity));
    }
    try {
        return put(out, const_cast<void*>(
                            create_instance<Queue>(Object{queue_methods(), "dstorage queue"}, id)));
    } catch (const std::bad_alloc&) {
        close_lane(id);
        return kOutOfMemory;
    }
}

HRESULT WINAPI set_staging_buffer_size(const Object*, std::uint32_t size) {
    G6SHIM_INFO("dstorage: staging buffer size: %u", size);
    Scheduler* const state = scheduler();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->lanes.staging = size;
    return kSucceeded;
}

HRESULT WINAPI status_result(Instance<StatusArray>* self, std::uint32_t index) {
    if (index >= self->state.size()) return kInvalidArgument;
    return self->state.at(index).load(std::memory_order_acquire);
}

BOOL WINAPI is_complete(Instance<StatusArray>* self, std::uint32_t index) {
    return status_result(self, index) != kPending;
}

void WINAPI enqueue_request(Instance<Queue>* self, const Request* request);

void queue_command(Instance<Queue>* self, Command command) {
    const std::uint64_t id = self->state.id;
    Scheduler* const state = scheduler();
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        const auto found = state->lanes.lanes.find(id);
        if (found == state->lanes.lanes.end() || found->second.closed) return;
        if (found->second.entries() + 1 < std::max<std::size_t>(found->second.capacity, 2)) break;
        state->finished.wait(lock);
    }
    auto& lane = state->lanes.lanes[id];
    lane.unsubmitted.push_back(std::move(command));
    if (lane.unsubmitted.size() >= std::max<std::size_t>(lane.capacity / 2, 1)) {
        state->lanes.submit(id);
        state->work.notify_all();
    }
}

void WINAPI enqueue_request(Instance<Queue>* self, const Request* request) {
    if (request == nullptr) return;
    queue_command(self, Command(std::in_place_type<Request>, *request));
}

void WINAPI enqueue_status(Instance<Queue>* self, Instance<StatusArray>* array,
                           std::uint32_t index) {
    if (array == nullptr) return;
    if (index >= array->state.size()) {
        G6SHIM_WARN("dstorage: status entry out of range: %u", index);
        return;
    }
    array->state.at(index).store(kPending, std::memory_order_release);
    add_ref_instance(array);
    queue_command(self, Command(std::in_place_type<Status>, array, index));
}

void WINAPI submit(Instance<Queue>* self) {
    const std::uint64_t id = self->state.id;
    Scheduler* const state = scheduler();
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto found = state->lanes.lanes.find(id);
    const std::size_t pending = found == state->lanes.lanes.end() ? 0 : found->second.unsubmitted.size();
    G6SHIM_TRACE("dstorage: submit: %zu commands", pending);
    state->lanes.submit(id);
    state->work.notify_all();
}

void WINAPI close(Instance<Queue>* self) { close_lane(self->state.id); }

const void* const* file_methods() {
    static const void* const table[kFileSlots] = {
        as_method(&missing<0>),          as_method(&add_ref_instance<File>),
        as_method(&release_instance<File>), as_method(&missing<3>),
        as_method(&missing<4>),          as_method(&missing<5>),
        as_method(&missing<6>),          as_method(&missing<7>),
    };
    return table;
}

const void* const* status_methods() {
    static const void* const table[kStatusSlots] = {
        as_method(&missing<0>),
        as_method(&add_ref_instance<StatusArray>),
        as_method(&release_instance<StatusArray>),
        as_method(&is_complete),
        as_method(&status_result),
    };
    return table;
}

const void* const* queue_methods() {
    static const void* const table[kQueueSlots] = {
        as_method(&missing<0>),
        as_method(&add_ref_instance<Queue>),
        as_method(&release_instance<Queue>),
        as_method(&enqueue_request),
        as_method(&enqueue_status),
        as_method(&missing<5>),
        as_method(&submit),
        as_method(&missing<7>),
        as_method(&close),
        as_method(&missing<9>),
        as_method(&missing<10>),
        as_method(&missing<11>),
    };
    return table;
}

const void* const* factory_methods() {
    static const void* const table[kFactorySlots] = {
        as_method(&missing<0>),           as_method(&add_ref),
        as_method(&add_ref),              as_method(&missing<3>),
        as_method(&open_file),            as_method(&create_status_array),
        as_method(&missing<6>),           as_method(&missing<7>),
        as_method(&set_staging_buffer_size), as_method(&missing<9>),
        as_method(&create_queue),         as_method(&missing<11>),
        as_method(&missing<12>),          as_method(&missing<13>),
        as_method(&missing<14>),          as_method(&missing<15>),
    };
    return table;
}

Object& factory_object() {
    static Object object{factory_methods(), "dstorage factory"};
    return object;
}

void work(Scheduler* state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        const std::optional<Job> job = state->lanes.start();
        if (!job) {
            state->work.wait(lock);
            continue;
        }
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        HRESULT result = kInvalidArgument;
        try {
            result = execute(job->request);
        } catch (const std::bad_alloc&) {
            result = kOutOfMemory;
        } catch (...) {
            result = kInvalidArgument;
        }
        lock.lock();
        state->lanes.finish(*job, result);
        state->lanes.count(*job, started);
        if (std::exchange(state->lanes.starved, false)) state->work.notify_all();
        state->finished.notify_all();
    }
}

void start_workers(Scheduler* state) {
    const unsigned available = std::thread::hardware_concurrency();
    const unsigned workers = available == 0 ? 4 : available;
    std::size_t started = 0;
    for (unsigned i = 0; i < workers; ++i) {
        try {
            std::thread([state] { work(state); }).detach();
            ++started;
        } catch (const std::system_error&) {
        }
    }
    G6SHIM_INFO("dstorage: workers: %zu", started);
}

}  // namespace

StatusArray::StatusArray(std::uint32_t capacity) : entries(capacity) {
    for (auto& entry : entries) entry.store(kSucceeded, std::memory_order_relaxed);
}

std::uint64_t staging_bytes(const Request& request) {
    const bool buffered =
        (request.options & (kOptionZlib | kOptionBcpack | kOptionPageDestination)) != 0;
    if (buffered && (request.options & kOptionMemorySource) == 0) return request.source_size;
    return 0;
}

Lane Lane::make(std::int8_t priority, std::uint16_t capacity) {
    Lane lane;
    lane.class_index = static_cast<std::size_t>(std::clamp<int>(priority, -1, 1) + 1);
    lane.capacity = capacity;
    return lane;
}

std::size_t Lane::entries() const {
    return unsubmitted.size() + reads.size() + statuses.size() + running.size();
}

void Lane::flush() {
    while (!statuses.empty()) {
        const std::uint64_t seq = statuses.front().first;
        if (!reads.empty() && reads.front().first < seq) return;
        if (!running.empty() && *running.begin() < seq) return;
        Status status = std::move(statuses.front().second);
        statuses.pop_front();
        const auto later = failures.lower_bound(seq);
        std::map<std::uint64_t, HRESULT> before(failures.begin(), later);
        failures.erase(failures.begin(), later);
        const HRESULT first = before.empty() ? kSucceeded : before.begin()->second;
        status.entry().store(first, std::memory_order_release);
    }
}

std::optional<Job> Lanes::start() {
    std::size_t chosen_class = 0;
    bool have_class = false;
    for (const auto& entry : lanes) {
        const Lane& lane = entry.second;
        if (lane.reads.empty()) continue;
        const std::size_t candidate = lane.class_index;
        if (!have_class || clock[candidate] < clock[chosen_class] ||
            (clock[candidate] == clock[chosen_class] && candidate < chosen_class)) {
            chosen_class = candidate;
            have_class = true;
        }
    }
    if (!have_class) return std::nullopt;
    std::uint64_t best_id = 0;
    std::uint64_t best_clock = 0;
    std::uint64_t best_seq = 0;
    bool have_lane = false;
    for (const auto& [id, lane] : lanes) {
        if (lane.reads.empty() || lane.class_index != chosen_class) continue;
        const std::uint64_t lane_clock = lane.clock;
        const std::uint64_t lane_seq = lane.reads.front().first;
        if (!have_lane ||
            std::tie(lane_clock, lane_seq, id) < std::tie(best_clock, best_seq, best_id)) {
            best_id = id;
            best_clock = lane_clock;
            best_seq = lane_seq;
            have_lane = true;
        }
    }
    if (!have_lane) return std::nullopt;
    const auto chosen = lanes.find(best_id);
    if (chosen == lanes.end()) return std::nullopt;
    Lane& lane = chosen->second;
    const std::uint64_t seq = lane.reads.front().first;
    const Request request = lane.reads.front().second;
    const std::uint64_t hold = staging_bytes(request);
    if (staged > 0 && staged + hold > this->staging) {
        starved = true;
        refused += 1;
        return std::nullopt;
    }
    lane.reads.pop_front();
    lane.running.insert(seq);
    staged += hold;
    const std::uint64_t size =
        std::max<std::uint64_t>(request.destination_size, request.source_size);
    const std::uint64_t weight = std::max<std::uint64_t>(size, 1);
    now = clock[chosen_class];
    clock[chosen_class] += weight * kPriorityCost[chosen_class];
    lane_now[chosen_class] = lane.clock;
    lane.clock += weight;
    return Job{best_id, seq, request, hold};
}

void Lanes::finish(const Job& job, HRESULT result) {
    staged -= job.staging;
    const auto found = lanes.find(job.lane);
    if (found == lanes.end()) return;
    Lane& lane = found->second;
    lane.running.erase(job.seq);
    if (result != kSucceeded) lane.failures.insert_or_assign(job.seq, result);
    lane.flush();
}

void Lanes::submit(std::uint64_t id) {
    const auto found = lanes.find(id);
    if (found == lanes.end()) return;
    const std::size_t chosen_class = found->second.class_index;
    bool waiting = false;
    for (const auto& entry : lanes)
        if (entry.second.class_index == chosen_class && !entry.second.reads.empty()) {
            waiting = true;
            break;
        }
    if (!waiting) clock[chosen_class] = std::max(clock[chosen_class], now);
    Lane& lane = found->second;
    if (lane.reads.empty()) lane.clock = std::max(lane.clock, lane_now[chosen_class]);
    std::vector<Command> commands = std::move(lane.unsubmitted);
    lane.unsubmitted.clear();
    for (auto& command : commands) {
        const std::uint64_t seq = next_seq;
        next_seq += 1;
        if (std::holds_alternative<Request>(command)) {
            lane.reads.emplace_back(seq, std::get<Request>(command));
        } else {
            lane.statuses.emplace_back(seq, std::move(std::get<Status>(command)));
        }
    }
    lane.flush();
}

void Lanes::count(const Job& job, std::chrono::steady_clock::time_point started) {
    const auto now_time = std::chrono::steady_clock::now();
    const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(now_time - started);
    done.requests += 1;
    done.bytes += job.request.destination_size;
    done.busy += spent;
    if ((job.request.options & kOptionBcpack) != 0) {
        bcpack.first += job.request.destination_size;
        bcpack.second += spent;
    }
    if (!since) since = started;
    const auto elapsed = now_time - *since;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    if (seconds >= 5.0) {
        std::size_t waiting = 0;
        for (const auto& entry : lanes) waiting += entry.second.reads.size();
        const double reading =
            static_cast<double>(reading_time().exchange(0, std::memory_order_relaxed)) / 1e9;
        const double busy = std::chrono::duration<double>(done.busy).count();
        const double bcpack_time = std::chrono::duration<double>(bcpack.second).count();
        G6SHIM_TRACE("dstorage: last %.1f s: %llu requests, %.1f MB/s, %.2f busy workers, %.2f "
                     "reading workers, %llu staging waits, %llu MB BCPack, %.2f BCPack workers, "
                     "%zu waiting",
                     seconds, static_cast<unsigned long long>(done.requests),
                     static_cast<double>(done.bytes) / seconds / 1e6, busy / seconds,
                     reading / seconds, static_cast<unsigned long long>(refused),
                     static_cast<unsigned long long>(bcpack.first / 1'000'000),
                     bcpack_time / seconds, waiting);
        done = WorkStats{};
        bcpack = {};
        since.reset();
        refused = 0;
    }
}

Scheduler* scheduler() {
    static Scheduler* instance = [] {
        auto* created = new Scheduler;
        start_workers(created);
        return created;
    }();
    return instance;
}

void close_lane(std::uint64_t id) {
    Scheduler* const state = scheduler();
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto found = state->lanes.lanes.find(id);
    if (found == state->lanes.lanes.end()) return;
    Lane& lane = found->second;
    lane.closed = true;
    lane.unsubmitted.clear();
    lane.reads.clear();
    lane.statuses.clear();
    for (;;) {
        const auto current = state->lanes.lanes.find(id);
        if (current == state->lanes.lanes.end() || current->second.running.empty()) break;
        state->finished.wait(lock);
    }
    state->lanes.lanes.erase(id);
    state->finished.notify_all();
}

void use_bcpack_tables(std::span<const std::uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(tables_mutex());
    TablesCache& cache = tables_cache();
    cache.loaded = true;
    BcpackError error{};
    auto parsed = Tables::parse(bytes, &error);
    if (parsed) {
        cache.tables = std::make_shared<const Tables>(std::move(*parsed));
        cache.why.clear();
    } else {
        cache.tables.reset();
        cache.why = error.reason != nullptr ? error.reason : "unknown";
    }
}

}  // namespace g6shim::xcrdapi

extern "C" HRESULT WINAPI g6shim_DStorageGetFactory(const g6shim::xcrdapi::Guid* iid,
                                                    void** out) {
    using namespace g6shim::xcrdapi;
    if (iid != nullptr && *iid == kFactoryInterface)
        return put(out, static_cast<void*>(&factory_object()));
    G6SHIM_WARN("DStorageGetFactory: unknown interface %s", guid_text(iid).c_str());
    return kNoInterface;
}
