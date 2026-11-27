// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xpackage.hpp"

#include "title.hpp"
#include "xasync.hpp"

#include <g6shim/log.hpp>

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <system_error>

namespace g6shim::xgameruntime::xpackage {

namespace {

using This = Object*;
using Handle = const void*;

const std::uint8_t kMonitor = 0;
std::uint8_t kInstall = 0;
std::uint8_t kMount = 0;

std::filesystem::path executable_folder() {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::filesystem::path path(module, module + length);
    path.remove_filename();
    return path;
}

std::uint64_t folder_bytes(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) return 0;
    std::uint64_t total = 0;
    for (const std::filesystem::directory_entry& entry : entries) {
        std::error_code kind_error;
        if (entry.is_directory(kind_error)) {
            total += folder_bytes(entry.path());
        } else if (!kind_error) {
            std::error_code size_error;
            const std::uintmax_t size = entry.file_size(size_error);
            if (!size_error) total += size;
        }
    }
    return total;
}

bool is_ours(const char* id) {
    const std::optional<std::string> value = text(id);
    return value && *value == identifier();
}

HResult current_identifier(This, std::size_t size, std::uint8_t* buffer) {
    return put_string(identifier(), size, buffer, nullptr);
}

bool is_packaged(This) { return true; }

HResult create_monitor(This, const char* id, std::uint32_t, const void*, std::uint32_t, const void*,
                       Handle* out) {
    if (!is_ours(id)) return kNoPackage;
    return put(out, monitor());
}

void close_monitor(This, Handle) {}

void progress(This, Handle, Progress* out) {
    const std::uint64_t bytes = package_bytes();
    if (out != nullptr) *out = Progress{bytes, bytes, bytes, true, true};
}

HResult register_progress(This, Handle, void*, const void*, std::uint64_t* out) { return put(out, token()); }

HResult find_availability(This, const char* id, std::uint32_t, const void*, std::uint32_t* out) {
    if (!is_ours(id)) return kNoPackage;
    return put(out, std::uint32_t{0});
}

HResult change_order(This, const char* id, std::uint32_t, const void*) { return is_ours(id) ? kOk : kNoPackage; }

HResult install_chunks_async(This, const char* id, std::uint32_t, const void*, std::uint32_t, bool,
                             xasync::AsyncBlock* block) {
    if (!is_ours(id)) return xasync::finished(block, &kInstall, kNoPackage);
    return xasync::finished_with(block, &kInstall, kOk, xasync::bytes(monitor()));
}

HResult install_chunks_result(This, xasync::AsyncBlock* block, Handle* out) {
    return xasync::result(block, &kInstall, out);
}

HResult estimate_download(This, const char* id, std::uint32_t, const void*, std::uint64_t* bytes,
                          bool* confirm) {
    if (!is_ours(id)) return kNoPackage;
    if (confirm != nullptr) *confirm = false;
    return put(bytes, std::uint64_t{0});
}

HResult uninstall_chunks(This, const char* id, std::uint32_t, const void*) {
    return is_ours(id) ? kOk : kNoPackage;
}

HResult mount_path(This, Handle, std::size_t, std::uint8_t*) { return kInvalidArg; }

void close_mount(This, Handle) {}

bool uninstall_package(This, const char* id) {
    G6SHIM_INFO("xgameruntime: package uninstall refused: %s", text(id).value_or("").c_str());
    return false;
}

HResult mount_async(This, const char* id, xasync::AsyncBlock* block) {
    G6SHIM_INFO("xgameruntime: package mount of %s: not installed", text(id).value_or("").c_str());
    return xasync::finished(block, &kMount, kNoPackage);
}

HResult mount_result(This, xasync::AsyncBlock* block, Handle* out) {
    return xasync::result(block, &kMount, out);
}

HResult enumerate(This, std::uint32_t kind, std::uint32_t, void*, const void*) {
    G6SHIM_TRACE("xgameruntime: packages of kind %u enumerated: none", kind);
    return kOk;
}

const std::array<const void*, 40> kVtable = {
    slot(&missing<0>),
    slot(&add_ref),
    slot(&add_ref),
    slot(&current_identifier),
    slot(&is_packaged),
    slot(&create_monitor),
    slot(&close_monitor),
    slot(&progress),
    slot(&missing<8>),
    slot(&register_progress),
    slot(&missing<10>),
    slot(&missing<11>),
    slot(&find_availability),
    slot(&missing<13>),
    slot(&change_order),
    slot(&missing<15>),
    slot(&install_chunks_async),
    slot(&install_chunks_result),
    slot(&estimate_download),
    slot(&uninstall_chunks),
    slot(&missing<20>),
    slot(&missing<21>),
    slot(&missing<22>),
    slot(&missing<23>),
    slot(&missing<24>),
    slot(&mount_path),
    slot(&close_mount),
    slot(&missing<27>),
    slot(&missing<28>),
    slot(&missing<29>),
    slot(&missing<30>),
    slot(&missing<31>),
    slot(&missing<32>),
    slot(&missing<33>),
    slot(&uninstall_package),
    slot(&missing<35>),
    slot(&missing<36>),
    slot(&mount_async),
    slot(&mount_result),
    slot(&enumerate),
};

}  // namespace

const Object kObject{kVtable.data(), "package"};

std::string identifier() { return title_identifier(title().id); }

const void* monitor() {
    static const std::uint8_t value = 0;
    return &value;
}

std::uint64_t package_bytes() {
    static const std::uint64_t bytes = folder_bytes(executable_folder());
    return bytes;
}

std::span<const Interface> interfaces() {
    static const Interface entries[] = {{kProvider, kIid2026, &kObject}};
    return entries;
}

}  // namespace g6shim::xgameruntime::xpackage

std::span<const g6shim::xgameruntime::Interface> g6shim::xgameruntime::xpackage_interfaces() {
    return xpackage::interfaces();
}
