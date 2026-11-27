// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xgamesave.hpp"

#include "storage.hpp"
#include "title.hpp"
#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <system_error>

namespace g6shim::xgameruntime::xgamesave {

namespace {

constexpr std::uint64_t kHashOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kHashPrime = 0x100000001b3ull;
constexpr std::uint64_t kWindowsEpoch = 116444736000000000ull;
constexpr std::size_t kMaxBlobBytes = std::size_t{64} << 20;
constexpr char kDefaultConfiguration[] = "default";
constexpr char kGoneExtension[] = ".gone";
constexpr char kDisplayFile[] = ".display";
constexpr std::uint64_t kXuid = 0x0009000000000001ull;
constexpr int kRetryAttempts = 10;
constexpr DWORD kRetryDelayMs = 1;

struct BlobSource {
    const std::string* name;
    const std::vector<std::uint8_t>* data;
    std::filesystem::path kept;
};

std::mutex& saves_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::error_code win_error() { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); }

bool transient(const std::error_code& error) {
    return error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION ||
           error.value() == ERROR_LOCK_VIOLATION;
}

void remove_tree(const std::filesystem::path& path, std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        std::filesystem::remove_all(path, error);
        if (!error) return;
        if (!transient(error)) return;
        Sleep(kRetryDelayMs);
    }
}

void make_directories(const std::filesystem::path& path, std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        std::filesystem::create_directories(path, error);
        if (!error) return;
        if (!transient(error)) return;
        Sleep(kRetryDelayMs);
    }
}

bool rename_path(const std::filesystem::path& from, const std::filesystem::path& to,
                 std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        std::filesystem::rename(from, to, error);
        if (!error) return true;
        if (!transient(error)) return false;
        Sleep(kRetryDelayMs);
    }
    return false;
}

bool link_file(const std::filesystem::path& from, const std::filesystem::path& to,
               std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        std::filesystem::create_hard_link(from, to, error);
        if (!error) return true;
        if (!transient(error)) return false;
        Sleep(kRetryDelayMs);
    }
    return false;
}

bool copy_into(const std::filesystem::path& from, const std::filesystem::path& to,
               std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
        if (!error) return true;
        if (!transient(error)) return false;
        Sleep(kRetryDelayMs);
    }
    return false;
}

HANDLE open_file(const std::filesystem::path& path, DWORD access, DWORD share, DWORD disposition,
                 std::error_code& error) {
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        const HANDLE file =
            CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            error.clear();
            return file;
        }
        error = win_error();
        if (!transient(error)) break;
        Sleep(kRetryDelayMs);
    }
    return INVALID_HANDLE_VALUE;
}

bool storage_full(const std::error_code& error) {
    return error == std::errc::no_space_on_device || error.value() == ERROR_DISK_FULL ||
           error.value() == ERROR_HANDLE_DISK_FULL;
}

bool not_found(const std::error_code& error) {
    return error == std::errc::no_such_file_or_directory || error.value() == ERROR_FILE_NOT_FOUND ||
           error.value() == ERROR_PATH_NOT_FOUND;
}

HResult io_error(const std::filesystem::path& dir, const std::error_code& error) {
    G6SHIM_WARN("xgameruntime: game save %ls: %s", dir.c_str(), error.message().c_str());
    return storage_full(error) ? kOutOfLocalStorage : kNoAccess;
}

bool valid_utf8(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        std::uint32_t point = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80) {
            ++at;
            continue;
        } else if ((lead & 0xe0) == 0xc0) {
            length = 2;
            point = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
            point = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
            point = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (at + length > text.size()) return false;
        for (std::size_t index = 1; index < length; ++index) {
            const unsigned char continuation = static_cast<unsigned char>(text[at + index]);
            if ((continuation & 0xc0) != 0x80) return false;
            point = (point << 6) | (continuation & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
        at += length;
    }
    return true;
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
    std::size_t at = 0;
    if (!text.empty() && text.front() == '+') at = 1;
    if (at == text.size()) return std::nullopt;
    std::uint64_t value = 0;
    for (; at < text.size(); ++at) {
        const char character = text[at];
        if (character < '0' || character > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

std::string without_nul(std::string value) {
    std::erase(value, '\0');
    return value;
}

bool read_file(const std::filesystem::path& path, std::size_t limit, std::vector<std::uint8_t>& out,
               std::error_code& error) {
    error.clear();
    const HANDLE file = open_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  OPEN_EXISTING, error);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == 0) {
        error = win_error();
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit) {
        error = std::make_error_code(std::errc::file_too_large);
        CloseHandle(file);
        return false;
    }
    out.assign(static_cast<std::size_t>(size.QuadPart), 0);
    std::size_t at = 0;
    while (at < out.size()) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(out.size() - at, 1u << 24));
        if (ReadFile(file, out.data() + at, wanted, &read, nullptr) == 0) {
            error = win_error();
            CloseHandle(file);
            return false;
        }
        if (read == 0) break;
        at += read;
    }
    CloseHandle(file);
    out.resize(at);
    return true;
}

bool write_file(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size,
                std::error_code& error) {
    error.clear();
    const HANDLE file = open_file(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  CREATE_ALWAYS, error);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t at = 0;
    while (at < size) {
        DWORD written = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size - at, 1u << 24));
        if (WriteFile(file, data + at, wanted, &written, nullptr) == 0 || written == 0) {
            error = win_error();
            CloseHandle(file);
            return false;
        }
        at += written;
    }
    if (FlushFileBuffers(file) == 0) {
        error = win_error();
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
}

bool write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data,
                std::error_code& error) {
    return write_file(path, data.data(), data.size(), error);
}

std::uint64_t modified_seconds(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) return 0;
    const ULARGE_INTEGER stamp{{data.ftLastWriteTime.dwLowDateTime, data.ftLastWriteTime.dwHighDateTime}};
    if (stamp.QuadPart < kWindowsEpoch) return 0;
    return static_cast<std::uint64_t>((stamp.QuadPart - kWindowsEpoch) / 10000000ull);
}

void step() {
    const CrashHook hook = crash_hook();
    if (hook != nullptr) hook();
}

std::filesystem::path gone_path(const std::filesystem::path& dir) {
    return dir.parent_path() / (dir.filename().string() + kGoneExtension);
}

std::vector<std::uint8_t> layout(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& blobs,
                                 std::uintptr_t base) {
    std::vector<std::uint8_t> out(blobs.size() * kBlobEntry, 0);
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        const auto& [name, data] = blobs[index];
        const std::uint64_t name_at = base + out.size();
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
        const std::uint64_t data_at = base + out.size();
        out.insert(out.end(), data.begin(), data.end());
        std::memcpy(out.data() + index * kBlobEntry, &name_at, sizeof name_at);
        const std::uint32_t length = static_cast<std::uint32_t>(data.size());
        std::memcpy(out.data() + index * kBlobEntry + 8, &length, sizeof length);
        std::memcpy(out.data() + index * kBlobEntry + 16, &data_at, sizeof data_at);
    }
    return out;
}

std::optional<std::vector<std::string>> names_of(const char* const* values, std::uint32_t count) {
    if (values == nullptr) return std::nullopt;
    std::vector<std::string> out;
    out.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) out.push_back(text(values[index]).value_or(""));
    return out;
}

std::optional<std::string> read_text(const std::filesystem::path& path, std::size_t limit) {
    std::vector<std::uint8_t> bytes;
    std::error_code error;
    if (!read_file(path, limit, bytes, error)) return std::nullopt;
    if (bytes.empty()) return std::string{};
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!valid_utf8(text)) return std::nullopt;
    return std::string(text);
}

std::error_code commit(const std::filesystem::path& dir, std::uint64_t number, std::string_view name,
                       std::string_view display, const std::vector<BlobSource>& blobs) {
    std::error_code error;
    const std::filesystem::path staged = dir / ("v" + std::to_string(number) + ".new");
    if (GetFileAttributesW(staged.c_str()) != INVALID_FILE_ATTRIBUTES) {
        remove_tree(staged, error);
        if (error) return error;
    }
    make_directories(staged, error);
    if (error) return error;
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        const std::filesystem::path path = staged / std::to_string(index);
        if (blobs[index].data != nullptr) {
            if (!write_file(path, *blobs[index].data, error)) return error;
        } else if (!link_file(blobs[index].kept, path, error)) {
            error.clear();
            if (!copy_into(blobs[index].kept, path, error)) return error;
        }
        step();
    }
    std::vector<std::string> names;
    names.reserve(blobs.size());
    for (const BlobSource& blob : blobs) names.push_back(*blob.name);
    const std::vector<std::uint8_t> manifest = manifest_bytes(name, display, names);
    if (!write_file(staged / kManifestFile, manifest, error)) return error;
    step();
    if (!rename_path(staged, dir / ("v" + std::to_string(number)), error)) return error;
    step();
    return {};
}

std::optional<std::filesystem::path> migrate(const std::filesystem::path& root, const std::string& name,
                                             const std::filesystem::path& old) {
    const std::filesystem::path dir = container_dir(root, name);
    std::error_code error;
    const std::optional<Version> existing = current_named(dir, name, error);
    if (error) return std::nullopt;
    if (!existing) {
        std::string display = read_text(old / kDisplayFile, kMaxManifestBytes).value_or("");
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
        std::filesystem::directory_iterator entries(old, error);
        if (error) return std::nullopt;
        const std::filesystem::directory_iterator end;
        while (entries != end) {
            const std::filesystem::path path = entries->path();
            std::optional<std::string> blob = decode_name(path.filename().string());
            std::vector<std::uint8_t> data;
            std::error_code read_error;
            if (blob && read_file(path, kMaxBlobBytes, data, read_error)) blobs.emplace_back(*blob, std::move(data));
            entries.increment(error);
            if (error) return std::nullopt;
        }
        std::sort(blobs.begin(), blobs.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        std::vector<BlobSource> sources;
        sources.reserve(blobs.size());
        for (const auto& [blob_name, data] : blobs) sources.push_back({&blob_name, &data, {}});
        error = commit(dir, 1, name, display, sources);
        if (error) return std::nullopt;
    }
    std::error_code remove_error;
    remove_tree(old, remove_error);
    if (remove_error) return std::nullopt;
    G6SHIM_INFO("xgameruntime: game save moved to the versioned layout: %s", name.c_str());
    return dir;
}

std::optional<std::uint64_t> version_number(std::string_view file) {
    if (file.size() < 2 || file.front() != 'v') return std::nullopt;
    const std::optional<std::uint64_t> number = parse_number(file.substr(1));
    if (!number) return std::nullopt;
    return number;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

using This = Object*;
using Handle = const void*;
using ContainerCallback = bool (*)(const ContainerInfo*, void*);
using BlobCallback = bool (*)(const BlobInfo*, void*);

std::uint8_t kInit = 0;
std::uint8_t kQuotaCall = 0;
std::uint8_t kDelete = 0;
std::uint8_t kRead = 0;
std::uint8_t kSubmit = 0;

HResult initialize_async(This, const void*, const char* config, bool, xasync::AsyncBlock* block) {
    const Title value = title();
    if (value.saves.parent_path().empty()) return xasync::finished(block, &kInit, kNoAccess);
    HResult error = kOk;
    std::optional<Provider> provider = open_provider(value.saves, text(config), error);
    if (!provider) return xasync::finished(block, &kInit, error);
    auto* handle = new Provider(std::move(*provider));
    return xasync::finished_with(block, &kInit, kOk, xasync::bytes(handle));
}

HResult initialize_result(This, xasync::AsyncBlock* block, Provider** out) {
    return xasync::result(block, &kInit, out);
}

void close_provider(This, Provider* provider) { delete provider; }

HResult quota_async(This, const Provider* provider, xasync::AsyncBlock* block) {
    if (provider == nullptr) return xasync::finished(block, &kQuotaCall, kInvalidArg);
    std::uint64_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(saves_mutex());
        remaining = remaining_bytes(provider->root);
    }
    const std::int64_t left = static_cast<std::int64_t>(remaining);
    return xasync::finished_with(block, &kQuotaCall, kOk, xasync::bytes(left));
}

HResult quota_result(This, xasync::AsyncBlock* block, std::int64_t* out) {
    return xasync::result(block, &kQuotaCall, out);
}

HResult delete_async(This, const Provider* provider, const char* name, xasync::AsyncBlock* block) {
    const std::optional<std::string> container = text(name);
    HResult result = kInvalidArg;
    if (provider != nullptr && container) {
        result = valid_container(*container) ? delete_container(provider->root, *container)
                                             : kInvalidContainerName;
    }
    return xasync::finished(block, &kDelete, result);
}

HResult delete_result(This, xasync::AsyncBlock* block) {
    return xasync::XAsyncGetResult(block, &kDelete, 0, nullptr, nullptr);
}

void report(const std::filesystem::path& root, const std::function<bool(const std::string&)>& keep, void* context,
            ContainerCallback callback) {
    struct Info {
        std::string name;
        std::string display;
        std::uint32_t blob_count;
        std::uint64_t total_size;
        std::int64_t last_modified;
    };
    std::vector<Info> infos;
    {
        std::lock_guard<std::mutex> lock(saves_mutex());
        for (const auto& [name, dir] : containers(root)) {
            if (!keep(name)) continue;
            std::error_code error;
            const std::optional<Version> version = current_named(dir, name, error);
            if (!version) continue;
            std::uint64_t size = 0;
            for (const auto& [blob_name, blob_size] : version->sizes()) size += blob_size;
            infos.push_back({without_nul(version->name), without_nul(version->display),
                             static_cast<std::uint32_t>(version->blobs.size()), size,
                             static_cast<std::int64_t>(modified_seconds(version->dir / kManifestFile))});
        }
    }
    for (const Info& info : infos) {
        const ContainerInfo entry{info.name.c_str(), info.display.c_str(), info.blob_count,
                                  info.total_size, info.last_modified, false};
        if (!callback(&entry, context)) break;
    }
}

HResult container_info(This, const Provider* provider, const char* name, void* context,
                       ContainerCallback callback) {
    const std::optional<std::string> container = text(name);
    if (provider == nullptr || !container || callback == nullptr) return kInvalidArg;
    if (!valid_container(*container)) return kInvalidContainerName;
    report(provider->root, [&container](const std::string& value) { return value == *container; }, context,
           callback);
    return kOk;
}

HResult enumerate_containers(This, const Provider* provider, void* context, ContainerCallback callback) {
    if (provider == nullptr || callback == nullptr) return kInvalidArg;
    report(provider->root, [](const std::string&) { return true; }, context, callback);
    return kOk;
}

HResult create_container(This, const Provider* provider, const char* name, Container** out) {
    const std::optional<std::string> container = text(name);
    if (provider == nullptr || !container) return kInvalidArg;
    if (!valid_container(*container)) return kInvalidContainerName;
    return put(out, new Container{*container, container_dir(provider->root, *container)});
}

void close_container(This, Container* container) { delete container; }

HResult enumerate_blobs(This, const Container* container, const char* prefix, void* context,
                        BlobCallback callback) {
    if (container == nullptr || callback == nullptr) return kInvalidArg;
    const std::string start = text(prefix).value_or("");
    std::vector<std::pair<std::string, std::uint64_t>> blobs;
    {
        std::lock_guard<std::mutex> lock(saves_mutex());
        std::error_code error;
        const std::optional<Version> version = current_named(container->dir, container->name, error);
        if (version) blobs = version->sizes();
    }
    for (const auto& [name, size] : blobs) {
        if (name.compare(0, start.size(), start) != 0) continue;
        const std::string entry_name = without_nul(name);
        const BlobInfo info{entry_name.c_str(), static_cast<std::uint32_t>(size)};
        if (!callback(&info, context)) break;
    }
    return kOk;
}

HResult read_blobs(This, const Container* container, const char* const* blob_names, std::uint32_t* count,
                   std::size_t size, std::uint8_t* out) {
    if (container == nullptr) return kInvalidArg;
    const std::uint32_t wanted = count != nullptr ? *count : 0;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
    const HResult result = read_container(*container, names_of(blob_names, wanted), blobs);
    if (result < 0) return result;
    const std::vector<std::uint8_t> bytes = layout(blobs, reinterpret_cast<std::uintptr_t>(out));
    if (out == nullptr || size < bytes.size()) return kBufferTooSmall;
    std::memcpy(out, bytes.data(), bytes.size());
    return put(count, static_cast<std::uint32_t>(blobs.size()));
}

HResult read_async(This, const Container* container, const char* const* blob_names, std::uint32_t count,
                   xasync::AsyncBlock* block) {
    if (container == nullptr) return xasync::finished(block, &kRead, kInvalidArg);
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
    const HResult result = read_container(*container, names_of(blob_names, count), blobs);
    if (result < 0) return xasync::finished(block, &kRead, result);
    return xasync::finished_with(block, &kRead, kOk, layout(blobs, 0));
}

HResult read_result(This, xasync::AsyncBlock* block, std::size_t size, std::uint8_t* out, std::uint32_t* count) {
    std::size_t used = 0;
    const HResult result = xasync::XAsyncGetResult(block, &kRead, size, out, &used);
    if (result == kNotSufficientBuffer) return kBufferTooSmall;
    if (result < 0) return result;
    std::uint64_t blobs = 0;
    if (used > 0 && out != nullptr) {
        std::uint64_t first = 0;
        std::memcpy(&first, out, sizeof first);
        blobs = first / kBlobEntry;
        if (blobs > used / kBlobEntry) blobs = used / kBlobEntry;
    }
    for (std::uint64_t index = 0; index < blobs; ++index) {
        for (const std::size_t field : {std::size_t{0}, std::size_t{16}}) {
            std::uint64_t value = 0;
            std::memcpy(&value, out + index * kBlobEntry + field, sizeof value);
            value += reinterpret_cast<std::uintptr_t>(out);
            std::memcpy(out + index * kBlobEntry + field, &value, sizeof value);
        }
    }
    return put(count, static_cast<std::uint32_t>(blobs));
}

HResult create_update(This, const Container* container, const char* display, Update** out) {
    if (container == nullptr) return kInvalidArg;
    return put(out, new Update{container->name, container->dir, text(display), {}});
}

void close_update(This, Update* update) { delete update; }

HResult write_blob(This, Update* update, const char* name, const std::uint8_t* data, std::size_t size) {
    const std::optional<std::string> blob = text(name);
    if (update == nullptr || !blob) return kInvalidArg;
    if (blob->empty() || (data == nullptr && size > 0)) return kInvalidArg;
    if (size > kMaxUpdate) return kUpdateTooBig;
    std::vector<std::uint8_t> value;
    if (size > 0) value.assign(data, data + size);
    update->changes.emplace_back(*blob, std::move(value));
    return kOk;
}

HResult delete_blob(This, Update* update, const char* name) {
    const std::optional<std::string> blob = text(name);
    if (update == nullptr || !blob || blob->empty()) return kInvalidArg;
    update->changes.emplace_back(*blob, std::nullopt);
    return kOk;
}

HResult submit_async(This, const Update* update, xasync::AsyncBlock* block) {
    HResult result = kInvalidArg;
    if (update != nullptr) {
        Update& value = const_cast<Update&>(*update);
        result = submit(value);
    }
    return xasync::finished(block, &kSubmit, result);
}

HResult submit_result(This, xasync::AsyncBlock* block) {
    return xasync::XAsyncGetResult(block, &kSubmit, 0, nullptr, nullptr);
}

const std::array<const void*, 30> kVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&missing<3>),
    slot(&initialize_async),
    slot(&initialize_result),
    slot(&close_provider),
    slot(&missing<7>),
    slot(&quota_async),
    slot(&quota_result),
    slot(&missing<10>),
    slot(&delete_async),
    slot(&delete_result),
    slot(&container_info),
    slot(&enumerate_containers),
    slot(&missing<15>),
    slot(&create_container),
    slot(&close_container),
    slot(&missing<18>),
    slot(&enumerate_blobs),
    slot(&read_blobs),
    slot(&read_async),
    slot(&read_result),
    slot(&create_update),
    slot(&close_update),
    slot(&write_blob),
    slot(&delete_blob),
    slot(&missing<27>),
    slot(&submit_async),
    slot(&submit_result),
};

}  // namespace

const Object kObject{kVtable.data(), "game save"};

CrashHook& crash_hook() {
    static CrashHook hook = nullptr;
    return hook;
}

std::filesystem::path Version::blob(std::size_t index) const { return dir / std::to_string(index); }

std::vector<std::pair<std::string, std::uint64_t>> Version::sizes() const {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    out.reserve(blobs.size());
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(blob(index), error);
        out.emplace_back(blobs[index], error ? 0 : static_cast<std::uint64_t>(size));
    }
    return out;
}

std::string file_name(std::string_view name) {
    std::string out;
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const char character : name) {
        const unsigned char byte = static_cast<unsigned char>(character);
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (plain) {
            out.push_back(character);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    return out;
}

std::optional<std::string> decode_name(std::string_view file) {
    if (file.find('.') != std::string_view::npos) return std::nullopt;
    std::string out;
    out.reserve(file.size());
    for (std::size_t at = 0; at < file.size(); ++at) {
        const char character = file[at];
        if (character != '%') {
            out.push_back(character);
            continue;
        }
        if (at + 2 >= file.size()) return std::nullopt;
        const auto digit = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        const int high = digit(file[at + 1]);
        const int low = digit(file[at + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
        at += 2;
    }
    if (!valid_utf8(out)) return std::nullopt;
    return out;
}

std::filesystem::path container_dir(const std::filesystem::path& root, std::string_view name) {
    std::uint64_t hash = kHashOffset;
    for (const char character : name) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= kHashPrime;
    }
    std::string readable = file_name(name);
    if (readable.size() > kReadableName) readable.resize(kReadableName);
    char suffix[24];
    std::snprintf(suffix, sizeof suffix, "~%016llX", static_cast<unsigned long long>(hash));
    return root / (readable + suffix);
}

bool valid_container(std::string_view name) {
    if (name.size() > kMaxContainerName) return false;
    const std::size_t slash = name.rfind('/');
    const std::string_view path = slash == std::string_view::npos ? std::string_view{} : name.substr(0, slash);
    const std::string_view file = slash == std::string_view::npos ? name : name.substr(slash + 1);
    const auto plain = [](unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
    };
    for (const char character : path) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!plain(byte) && byte != '_' && byte != '/') return false;
    }
    if (file.empty() || file.back() == '.') return false;
    if (file.find("..") != std::string_view::npos) return false;
    for (const char character : file) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (!plain(byte) && byte != '_' && byte != '.' && byte != '-') return false;
    }
    return true;
}

std::vector<std::uint8_t> manifest_bytes(std::string_view name, std::string_view display,
                                         const std::vector<std::string>& blobs) {
    std::vector<std::uint8_t> out;
    const auto append = [&out](std::string_view value) {
        const std::uint32_t length = static_cast<std::uint32_t>(value.size());
        out.resize(out.size() + sizeof length);
        std::memcpy(out.data() + out.size() - sizeof length, &length, sizeof length);
        out.insert(out.end(), value.begin(), value.end());
    };
    append(name);
    append(display);
    for (const std::string& blob : blobs) append(blob);
    return out;
}

std::optional<std::vector<std::string>> parse_manifest(std::span<const std::uint8_t> bytes) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 4) return std::nullopt;
        std::uint32_t length = 0;
        std::memcpy(&length, bytes.data() + at, sizeof length);
        at += 4;
        if (length > bytes.size() - at) return std::nullopt;
        const std::string_view text(reinterpret_cast<const char*>(bytes.data() + at), length);
        if (!valid_utf8(text)) return std::nullopt;
        out.emplace_back(text);
        at += length;
    }
    if (out.size() < 2) return std::nullopt;
    return out;
}

std::optional<Version> current(const std::filesystem::path& dir, std::error_code& error) {
    error.clear();
    std::vector<std::uint64_t> committed;
    std::error_code scan_error;
    std::filesystem::directory_iterator entries(dir, scan_error);
    if (scan_error) {
        error = scan_error;
        return std::nullopt;
    }
    const std::filesystem::directory_iterator end;
    while (entries != end) {
        const std::filesystem::path path = entries->path();
        const std::string file = path.filename().string();
        std::error_code kind_error;
        const bool directory = entries->is_directory(kind_error);
        if (!kind_error && ends_with(file, ".new")) {
            std::error_code remove_error;
            remove_tree(path, remove_error);
            if (remove_error) {
                error = remove_error;
                return std::nullopt;
            }
        } else if (!kind_error && directory) {
            if (const std::optional<std::uint64_t> number = version_number(file)) committed.push_back(*number);
        }
        entries.increment(scan_error);
        if (scan_error) {
            error = scan_error;
            return std::nullopt;
        }
    }
    if (committed.empty()) return std::nullopt;
    std::sort(committed.begin(), committed.end());
    const std::uint64_t number = committed.back();
    for (std::size_t index = 0; index + 1 < committed.size(); ++index) {
        std::error_code remove_error;
        remove_tree(dir / ("v" + std::to_string(committed[index])), remove_error);
        if (remove_error) {
            error = remove_error;
            return std::nullopt;
        }
    }
    std::vector<std::uint8_t> bytes;
    error.clear();
    std::error_code read_error;
    if (!read_file(dir / ("v" + std::to_string(number)) / kManifestFile, kMaxManifestBytes, bytes, read_error)) {
        error = read_error;
        return std::nullopt;
    }
    std::optional<std::vector<std::string>> names = parse_manifest(bytes);
    if (!names) {
        error = std::make_error_code(std::errc::invalid_argument);
        return std::nullopt;
    }
    Version version;
    version.dir = dir / ("v" + std::to_string(number));
    version.number = number;
    version.name = names->at(0);
    version.display = names->at(1);
    version.blobs.assign(names->begin() + 2, names->end());
    return version;
}

std::optional<Version> current_named(const std::filesystem::path& dir, std::string_view name,
                                     std::error_code& error) {
    std::optional<Version> version = current(dir, error);
    if (error) {
        if (not_found(error)) error.clear();
        return std::nullopt;
    }
    if (!version || version->name != name) return std::nullopt;
    return version;
}

std::vector<std::pair<std::string, std::filesystem::path>> containers(const std::filesystem::path& root) {
    std::vector<std::pair<std::string, std::filesystem::path>> out;
    std::error_code scan_error;
    std::filesystem::directory_iterator entries(root, scan_error);
    if (scan_error) return out;
    const std::filesystem::directory_iterator end;
    while (entries != end) {
        const std::filesystem::path path = entries->path();
        const std::string file = path.filename().string();
        std::error_code kind_error;
        if (entries->is_directory(kind_error) && !kind_error) {
            if (ends_with(file, kGoneExtension)) {
                std::error_code ignored;
                remove_tree(path, ignored);
            } else if (file.find('~') != std::string::npos) {
                std::error_code error;
                if (const std::optional<Version> version = current(path, error)) out.emplace_back(version->name, path);
            } else if (const std::optional<std::string> name = decode_name(file)) {
                if (const std::optional<std::filesystem::path> dir = migrate(root, *name, path))
                    out.emplace_back(*name, *dir);
            }
        }
        entries.increment(scan_error);
        if (scan_error) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<Provider> open_provider(const std::filesystem::path& saves, std::optional<std::string> config,
                                      HResult& error) {
    std::string name = config && !config->empty() ? *config : kDefaultConfiguration;
    char xuid[24];
    std::snprintf(xuid, sizeof xuid, "%016llX", static_cast<unsigned long long>(kXuid));
    const std::filesystem::path root = saves / xuid / file_name(name);
    std::error_code create_error;
    make_directories(root, create_error);
    if (create_error) {
        error = kNoAccess;
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(saves_mutex());
        containers(root);
    }
    error = kOk;
    return Provider{root};
}

std::uint64_t save_used_bytes(const std::filesystem::path& root) {
    std::uint64_t total = 0;
    for (const auto& [name, dir] : containers(root)) {
        std::error_code error;
        const std::optional<Version> version = current_named(dir, name, error);
        if (!version) continue;
        for (const auto& [blob_name, size] : version->sizes()) total += size;
    }
    return total;
}

std::uint64_t remaining_bytes(const std::filesystem::path& root) {
    const std::uint64_t used = save_used_bytes(root);
    const std::uint64_t left = kQuota > used ? kQuota - used : 0;
    return std::min(left, storage::disk_free(root));
}

std::optional<HResult> no_room(std::uint64_t written, std::uint64_t replaced, std::uint64_t used,
                               std::uint64_t free) {
    const std::uint64_t quota_left = kQuota > used ? kQuota - used : 0;
    const std::uint64_t allowed = quota_left > UINT64_MAX - replaced ? UINT64_MAX : quota_left + replaced;
    if (written > allowed) return kQuotaExceeded;
    if (written > free) return kOutOfLocalStorage;
    return std::nullopt;
}

HResult submit(Update& update) {
    std::lock_guard<std::mutex> lock(saves_mutex());
    std::error_code error;
    std::optional<Version> version = current(update.dir, error);
    if (error) {
        if (!not_found(error)) return io_error(update.dir, error);
        error.clear();
        version.reset();
    }
    if (version && version->name != update.name) return kNoAccess;
    std::vector<BlobSource> blobs;
    if (version) {
        for (std::size_t index = 0; index < version->blobs.size(); ++index)
            blobs.push_back({&version->blobs[index], nullptr, version->blob(index)});
    }
    for (const auto& [name, data] : update.changes) {
        std::erase_if(blobs, [&name](const BlobSource& blob) { return *blob.name == name; });
        if (data) blobs.push_back({&name, &*data, {}});
    }
    std::sort(blobs.begin(), blobs.end(),
              [](const BlobSource& left, const BlobSource& right) { return *left.name < *right.name; });
    std::uint64_t written = 0;
    for (const BlobSource& blob : blobs) {
        if (blob.data != nullptr) written += blob.data->size();
    }
    if (written > kMaxUpdate) return kUpdateTooBig;
    std::uint64_t replaced = 0;
    if (version) {
        for (const auto& [name, size] : version->sizes()) {
            for (const auto& change : update.changes) {
                if (change.first == name) {
                    replaced += size;
                    break;
                }
            }
        }
    }
    const std::filesystem::path root = update.dir.parent_path().empty() ? update.dir : update.dir.parent_path();
    if (const std::optional<HResult> failure = no_room(written, replaced, save_used_bytes(root),
                                                       storage::disk_free(root)))
        return *failure;
    std::string display_value;
    if (update.display) {
        display_value = *update.display;
    } else if (version) {
        display_value = version->display;
    }
    const std::uint64_t number = version ? version->number + 1 : 1;
    error = commit(update.dir, number, update.name, display_value, blobs);
    if (error) return io_error(update.dir, error);
    if (version) {
        std::error_code ignored;
        remove_tree(version->dir, ignored);
        step();
    }
    return kOk;
}

HResult delete_container(const std::filesystem::path& root, std::string_view name) {
    std::lock_guard<std::mutex> lock(saves_mutex());
    const std::filesystem::path dir = container_dir(root, name);
    std::error_code error;
    const std::optional<Version> version = current_named(dir, name, error);
    if (error) return io_error(dir, error);
    if (!version) return kOk;
    const std::filesystem::path gone = gone_path(dir);
    {
        std::error_code ignored;
        remove_tree(gone, ignored);
    }
    step();
    rename_path(dir, gone, error);
    if (error) return io_error(dir, error);
    step();
    {
        std::error_code ignored;
        remove_tree(gone, ignored);
    }
    return kOk;
}

HResult read_container(const Container& container, const std::optional<std::vector<std::string>>& names,
                       std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& blobs) {
    std::lock_guard<std::mutex> lock(saves_mutex());
    std::error_code error;
    const std::optional<Version> version = current_named(container.dir, container.name, error);
    if (error) return io_error(container.dir, error);
    if (!version) return kBlobNotFound;
    const std::vector<std::string> wanted = names.value_or(version->blobs);
    blobs.clear();
    for (const std::string& name : wanted) {
        std::size_t index = 0;
        while (index < version->blobs.size() && version->blobs[index] != name) ++index;
        if (index == version->blobs.size()) return kBlobNotFound;
        std::vector<std::uint8_t> data;
        if (!read_file(version->blob(index), kMaxBlobBytes, data, error)) return io_error(container.dir, error);
        blobs.emplace_back(name, std::move(data));
    }
    return kOk;
}

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kProvider, &kObject}, {kProvider, kIid2026, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::xgamesave

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xgamesave_interfaces() {
    return xgamesave::interfaces();
}
