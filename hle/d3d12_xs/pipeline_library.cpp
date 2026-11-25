// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline_library.hpp"

#include "host.hpp"
#include "shader_cache.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/emit/container.hpp>

#include <cstring>
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
std::set<std::string> g_stored;
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

struct Parts {
    std::vector<std::uint8_t> bytes;

    void add(std::span<const std::uint8_t> value) {
        const std::uint64_t length = value.size();
        for (std::size_t i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void add(std::string_view text) {
        add(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    template <typename T>
    void add_pod(const T& value) {
        add(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&value), sizeof value));
    }

    void add_text(const char* text) {
        if (text == nullptr) {
            add(std::span<const std::uint8_t>{});
            return;
        }
        add(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)));
    }

    void add_bytecode(const D3D12_SHADER_BYTECODE& code) {
        if (code.pShaderBytecode == nullptr || code.BytecodeLength == 0) {
            add(std::span<const std::uint8_t>{});
            return;
        }
        add(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(code.pShaderBytecode), code.BytecodeLength));
    }

    void add_cached(const D3D12_CACHED_PIPELINE_STATE& cached) {
        if (cached.pCachedBlob == nullptr || cached.CachedBlobSizeInBytes == 0) {
            add(std::span<const std::uint8_t>{});
            return;
        }
        add(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(cached.pCachedBlob), cached.CachedBlobSizeInBytes));
    }

    void add_input(const D3D12_INPUT_LAYOUT_DESC& layout) {
        const UINT count = layout.pInputElementDescs == nullptr ? 0 : layout.NumElements;
        add_pod(count);
        for (UINT i = 0; i < count; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& element = layout.pInputElementDescs[i];
            add_text(element.SemanticName);
            add_pod(element.SemanticIndex);
            add_pod(element.Format);
            add_pod(element.InputSlot);
            add_pod(element.AlignedByteOffset);
            add_pod(element.InputSlotClass);
            add_pod(element.InstanceDataStepRate);
        }
    }

    void add_stream_output(const D3D12_STREAM_OUTPUT_DESC& output) {
        const UINT entries = output.pSODeclaration == nullptr ? 0 : output.NumEntries;
        const UINT strides = output.pBufferStrides == nullptr ? 0 : output.NumStrides;
        add_pod(entries);
        for (UINT i = 0; i < entries; ++i) {
            const D3D12_SO_DECLARATION_ENTRY& entry = output.pSODeclaration[i];
            add_pod(entry.Stream);
            add_text(entry.SemanticName);
            add_pod(entry.SemanticIndex);
            add_pod(entry.StartComponent);
            add_pod(entry.ComponentCount);
            add_pod(entry.OutputSlot);
        }
        add_pod(strides);
        if (strides > 0) {
            add(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(output.pBufferStrides),
                                              static_cast<std::size_t>(strides) * sizeof(UINT)));
        }
        add_pod(output.RasterizedStream);
    }

    void add_views(const D3D12_VIEW_INSTANCING_DESC& views) {
        const UINT count = views.pViewInstanceLocations == nullptr ? 0 : views.ViewInstanceCount;
        add_pod(count);
        for (UINT i = 0; i < count; ++i) add_pod(views.pViewInstanceLocations[i]);
        add_pod(views.Flags);
    }

    void add_root(ID3D12RootSignature* root) {
        const std::uint8_t present = root != nullptr ? 1 : 0;
        add(std::span<const std::uint8_t>(&present, 1));
    }
};

std::string digest(std::span<const std::uint8_t> bytes) {
    const std::array<std::uint8_t, 16> hashed = g6shim::shader::emit::hash(bytes);
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t byte : hashed) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::string named(std::string_view kind, std::span<const std::uint8_t> root, const Parts& state) {
    Parts all;
    all.add(kind);
    all.add(root);
    all.add(state.bytes);
    return digest(all.bytes);
}

template <typename T>
std::size_t append_raw(Parts& out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const std::uint8_t* cursor,
                       const std::uint8_t* end) {
    const std::size_t bytes = stream_subobject_bytes<T>();
    if (static_cast<std::size_t>(end - cursor) < bytes) return 0;
    out.add_pod(type);
    out.add(std::span<const std::uint8_t>(cursor + stream_subobject_value<T>(), sizeof(T)));
    return bytes;
}

template <typename T, typename Read>
std::size_t append_ref(Parts& out, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const std::uint8_t* cursor,
                       const std::uint8_t* end, Read read) {
    const std::size_t bytes = stream_subobject_bytes<T>();
    if (static_cast<std::size_t>(end - cursor) < bytes) return 0;
    out.add_pod(type);
    read(*reinterpret_cast<const T*>(cursor + stream_subobject_value<T>()));
    return bytes;
}

std::size_t append_subobject(Parts& out, const std::uint8_t* cursor, const std::uint8_t* end) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) return 0;
    const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(cursor);
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        return append_ref<ID3D12RootSignature*>(out, type, cursor, end, [&](ID3D12RootSignature* root) { out.add_root(root); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        return append_ref<D3D12_SHADER_BYTECODE>(out, type, cursor, end,
                                                 [&](const D3D12_SHADER_BYTECODE& code) { out.add_bytecode(code); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        return append_ref<D3D12_STREAM_OUTPUT_DESC>(out, type, cursor, end,
                                                    [&](const D3D12_STREAM_OUTPUT_DESC& output) { out.add_stream_output(output); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        return append_raw<D3D12_BLEND_DESC>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        return append_raw<UINT>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        return append_raw<D3D12_RASTERIZER_DESC>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        return append_raw<D3D12_DEPTH_STENCIL_DESC>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        return append_ref<D3D12_INPUT_LAYOUT_DESC>(out, type, cursor, end,
                                                   [&](const D3D12_INPUT_LAYOUT_DESC& layout) { out.add_input(layout); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        return append_raw<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        return append_raw<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        return append_raw<D3D12_RT_FORMAT_ARRAY>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        return append_raw<DXGI_FORMAT>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        return append_raw<DXGI_SAMPLE_DESC>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        return append_raw<UINT>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return append_ref<D3D12_CACHED_PIPELINE_STATE>(out, type, cursor, end,
                                                       [&](const D3D12_CACHED_PIPELINE_STATE& cached) { out.add_cached(cached); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        return append_raw<D3D12_PIPELINE_STATE_FLAGS>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        return append_raw<D3D12_DEPTH_STENCIL_DESC1>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        return append_ref<D3D12_VIEW_INSTANCING_DESC>(out, type, cursor, end,
                                                      [&](const D3D12_VIEW_INSTANCING_DESC& views) { out.add_views(views); });
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
        return append_raw<D3D12_DEPTH_STENCIL_DESC2>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
        return append_raw<D3D12_RASTERIZER_DESC1>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
        return append_raw<D3D12_RASTERIZER_DESC2>(out, type, cursor, end);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID:
        return 0;
    }
    return 0;
}

void add_stream(Parts& out, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    if (desc.pPipelineStateSubobjectStream == nullptr || desc.SizeInBytes == 0) return;
    const auto* begin = static_cast<const std::uint8_t*>(desc.pPipelineStateSubobjectStream);
    const auto* cursor = begin;
    const auto* end = begin + desc.SizeInBytes;
    while (cursor < end) {
        const std::size_t taken = append_subobject(out, cursor, end);
        if (taken == 0) {
            out.add(std::span<const std::uint8_t>(cursor, static_cast<std::size_t>(end - cursor)));
            return;
        }
        cursor += taken;
    }
}

void add_graphics(Parts& out, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    out.add_root(desc.pRootSignature);
    out.add_bytecode(desc.VS);
    out.add_bytecode(desc.PS);
    out.add_bytecode(desc.DS);
    out.add_bytecode(desc.HS);
    out.add_bytecode(desc.GS);
    out.add_stream_output(desc.StreamOutput);
    out.add_pod(desc.BlendState);
    out.add_pod(desc.SampleMask);
    out.add_pod(desc.RasterizerState);
    out.add_pod(desc.DepthStencilState);
    out.add_input(desc.InputLayout);
    out.add_pod(desc.IBStripCutValue);
    out.add_pod(desc.PrimitiveTopologyType);
    out.add_pod(desc.NumRenderTargets);
    out.add_pod(desc.RTVFormats);
    out.add_pod(desc.DSVFormat);
    out.add_pod(desc.SampleDesc);
    out.add_pod(desc.NodeMask);
    out.add_cached(desc.CachedPSO);
    out.add_pod(desc.Flags);
}

void add_compute(Parts& out, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
    out.add_root(desc.pRootSignature);
    out.add_bytecode(desc.CS);
    out.add_pod(desc.NodeMask);
    out.add_cached(desc.CachedPSO);
    out.add_pod(desc.Flags);
}

static_assert(stream_subobject_bytes<D3D12_BLEND_DESC>() == 336);
static_assert(stream_subobject_value<D3D12_BLEND_DESC>() == 4);
static_assert(stream_subobject_bytes<D3D12_SHADER_BYTECODE>() == 24);
static_assert(stream_subobject_value<D3D12_SHADER_BYTECODE>() == 8);
static_assert(stream_subobject_bytes<ID3D12RootSignature*>() == 16);
static_assert(stream_subobject_value<ID3D12RootSignature*>() == 8);
static_assert(stream_subobject_bytes<std::uint32_t>() == 8);
static_assert(stream_subobject_value<std::uint32_t>() == 4);
static_assert(stream_subobject_bytes<D3D12_RASTERIZER_DESC>() == 48);
static_assert(stream_subobject_bytes<D3D12_DEPTH_STENCIL_DESC>() == 56);
static_assert(stream_subobject_bytes<D3D12_RT_FORMAT_ARRAY>() == 40);
static_assert(stream_subobject_bytes<DXGI_FORMAT>() == 8);
static_assert(stream_subobject_bytes<DXGI_SAMPLE_DESC>() == 16);

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

std::string name(std::string_view kind, std::span<const std::uint8_t> root, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    Parts state;
    add_graphics(state, desc);
    return named(kind, root, state);
}

std::string name(std::string_view kind, std::span<const std::uint8_t> root, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
    Parts state;
    add_compute(state, desc);
    return named(kind, root, state);
}

std::string name(std::string_view kind, std::span<const std::uint8_t> root, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    Parts state;
    add_stream(state, desc);
    return named(kind, root, state);
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
            g_stored.insert(name);
            loaded.fetch_add(1, std::memory_order_relaxed);
            return S_OK;
        }
    }
    const HRESULT created = create(out);
    if (FAILED(created)) return created;
    made.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_library_mutex);
    if (g_stored.contains(name)) return S_OK;
    if (SUCCEEDED(found->library->StorePipeline(wide_name.c_str(), *out))) {
        ++found->added;
        g_stored.insert(name);
    }
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
