// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_library.hpp"

#include "host.hpp"
#include "shader_cache.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/emit/container.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace g6shim::d3d12_xs::pipeline_library {

namespace {

constexpr std::chrono::seconds kSaveEvery{20};
constexpr std::size_t kMaxLibraryBytes = 1u << 30;

std::once_flag g_directory_once;
std::optional<std::filesystem::path> g_directory;

std::mutex g_library_mutex;
std::once_flag g_library_once;
std::optional<Library> g_library;

std::mutex g_used_mutex;
std::optional<std::set<std::string>> g_used;
std::atomic<bool> g_used_changed{false};

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxLibraryBytes) return std::nullopt;
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !in.read(bytes.data(), size)) return std::nullopt;
    return bytes;
}

bool write_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::wstring wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::set<std::string>& used_locked() {
    if (!g_used) {
        std::set<std::string> listed;
        if (const auto dir = directory()) {
            if (const auto text = read_file(*dir / kEarlyFile); text) {
                std::size_t at = 0;
                while (at < text->size()) {
                    const std::size_t end = text->find('\n', at);
                    const std::size_t length = (end == std::string::npos ? text->size() : end) - at;
                    if (length > 0) listed.insert(text->substr(at, length));
                    at = end == std::string::npos ? text->size() : end + 1;
                }
            }
        }
        G6SHIM_INFO("d3d12_xs: %zu title pipelines to start early", listed.size());
        g_used = std::move(listed);
    }
    return *g_used;
}

Library* library() {
    std::call_once(g_library_once, [] {
        host::Host* gpu = host::host();
        if (gpu == nullptr) return;
        Microsoft::WRL::ComPtr<ID3D12Device1> device;
        if (FAILED(gpu->device()->QueryInterface(IID_PPV_ARGS(&device)))) return;
        std::vector<std::uint8_t> blob;
        if (const auto dir = directory()) {
            if (const auto file = read_file(*dir / kLibraryFile); file) {
                blob.reserve(file->size());
                for (const char byte : *file) blob.push_back(static_cast<std::uint8_t>(byte));
            }
        }
        g_library = open(device.Get(), std::move(blob));
        if (!g_library) G6SHIM_WARN("d3d12_xs: no host pipeline library: pipelines are not kept");
    });
    return g_library ? &*g_library : nullptr;
}

}  // namespace

std::size_t Library::serialized_size() const {
    return library == nullptr ? 0 : library->GetSerializedSize();
}

std::optional<std::filesystem::path> directory() {
    std::call_once(g_directory_once, [] {
        const auto root = shader_cache::root_directory();
        if (!root) return;
        g_directory = *root / L"pipelines";
    });
    return g_directory;
}

void use_directory(const std::filesystem::path& dir) {
    std::call_once(g_directory_once, [&dir] {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        g_directory = dir;
    });
}

std::optional<Library> open(ID3D12Device1* device, std::vector<std::uint8_t> blob) {
    if (device == nullptr) return std::nullopt;
    const auto make = [device](const std::vector<std::uint8_t>& bytes) {
        Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> base;
        const void* data = bytes.empty() ? nullptr : bytes.data();
        if (FAILED(device->CreatePipelineLibrary(data, bytes.size(), IID_PPV_ARGS(&base)))) return Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1>{};
        Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> one;
        if (FAILED(base.As(&one))) return Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1>{};
        return one;
    };
    if (!blob.empty()) {
        if (auto library = make(blob); library != nullptr) {
            G6SHIM_INFO("d3d12_xs: host pipeline library loaded (%zu bytes)", blob.size());
            return Library{std::move(library), std::move(blob), 0};
        }
        G6SHIM_INFO("d3d12_xs: host pipeline library rejected; starting an empty one");
    } else {
        G6SHIM_INFO("d3d12_xs: no host pipeline library yet; starting an empty one");
    }
    auto library = make({});
    if (library == nullptr) return std::nullopt;
    return Library{std::move(library), {}, 0};
}

std::string name(std::string_view kind, std::span<const std::uint8_t> root,
                 std::span<const std::span<const std::uint8_t>> code, std::string_view state) {
    std::vector<std::uint8_t> bytes;
    const auto part = [&bytes](std::span<const std::uint8_t> value) {
        const std::uint64_t length = value.size();
        for (std::size_t i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    part(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(kind.data()), kind.size()));
    part(root);
    for (const auto& each : code) part(each);
    part(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(state.data()), state.size()));
    const std::array<std::uint8_t, 16> digest = g6shim::shader::emit::hash(bytes);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

HRESULT pipeline(const std::string& name, const Load& load, const Create& create, ID3D12PipelineState** out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    const std::wstring wide_name = wide(name);
    Library* found = library();
    if (found == nullptr) {
        made.fetch_add(1, std::memory_order_relaxed);
        return create(out);
    }
    {
        std::lock_guard<std::mutex> lock(g_library_mutex);
        if (SUCCEEDED(load(found->library.Get(), wide_name.c_str()))) {
            loaded.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
    }
    const HRESULT created = create(out);
    if (FAILED(created)) return created;
    made.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_library_mutex);
    if (SUCCEEDED(found->library->StorePipeline(wide_name.c_str(), *out))) ++found->added;
    return S_OK;
}

void save(bool now) {
    static std::mutex last_mutex;
    static std::optional<std::chrono::steady_clock::time_point> last;
    const auto dir = directory();
    Library* found = library();
    if (!dir || found == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(last_mutex);
        const auto point = std::chrono::steady_clock::now();
        if (!now && last && point - *last < kSaveEvery) return;
        last = point;
    }
    std::vector<std::uint8_t> blob;
    {
        std::lock_guard<std::mutex> lock(g_library_mutex);
        if (found->added == 0 && !g_used_changed.exchange(false)) return;
        const SIZE_T size = found->library->GetSerializedSize();
        blob.resize(size);
        if (FAILED(found->library->Serialize(blob.data(), blob.size()))) {
            G6SHIM_WARN("d3d12_xs: host pipeline library not serialised");
            return;
        }
        found->added = 0;
    }
    std::vector<std::string> used;
    {
        std::lock_guard<std::mutex> lock(g_used_mutex);
        if (g_used) used.assign(g_used->begin(), g_used->end());
    }
    std::string lines;
    for (const std::string& key : used) {
        if (!lines.empty()) lines.push_back('\n');
        lines += key;
    }
    std::error_code error;
    std::filesystem::create_directories(*dir, error);
    std::vector<std::uint8_t> keys;
    keys.reserve(lines.size());
    for (const char c : lines) keys.push_back(static_cast<std::uint8_t>(c));
    const bool written = write_atomically(*dir / kLibraryFile, blob) && write_atomically(*dir / kEarlyFile, keys);
    if (!written) {
        G6SHIM_WARN("d3d12_xs: host pipeline library not saved under %s", dir->string().c_str());
        return;
    }
    G6SHIM_INFO("d3d12_xs: host pipeline library saved (%zu bytes, %zu early, %llu made, %llu loaded, %llu started "
                "early)",
                blob.size(), used.size(),
                static_cast<unsigned long long>(made.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(loaded.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(early.load(std::memory_order_relaxed)));
}

bool wanted_early(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_used_mutex);
    return used_locked().contains(key);
}

void note_used(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_used_mutex);
    std::set<std::string>& used = used_locked();
    if (used.size() < kEarlyMax && used.insert(key).second) g_used_changed.store(true, std::memory_order_relaxed);
}

}  // namespace g6shim::d3d12_xs::pipeline_library
