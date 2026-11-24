// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/validator.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace g6shim::shader::emit {

namespace {

struct Guid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};
};

constexpr Guid kClsidDxcValidator{0x8ca3e215u,
                                  0xf728,
                                  0x4cf3,
                                  {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
constexpr Guid kIidIdxcValidator{0xa6e82bd2u,
                                 0x1fd7,
                                 0x4826,
                                 {0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a}};

constexpr std::uint32_t kInPlaceEdit = 1;
constexpr HRESULT kNoInterface = static_cast<HRESULT>(0x8000'4002u);

constexpr std::array<std::uint8_t, 16> kIUnknown = {0, 0, 0, 0, 0, 0, 0, 0,
                                                    0xc0, 0, 0, 0, 0, 0, 0, 0x46};
constexpr std::array<std::uint8_t, 16> kIdxcBlob = {0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40,
                                                    0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02};

struct Blob {
    const void* const* vtable = nullptr;
    std::uint8_t* data = nullptr;
    std::size_t length = 0;
};

using QueryInterface = HRESULT(__stdcall*)(Blob*, const std::array<std::uint8_t, 16>*, void**);
using AddRef = std::uint32_t(__stdcall*)(Blob*);
using BufferPointer = void*(__stdcall*)(Blob*);
using BufferSize = std::size_t(__stdcall*)(Blob*);

struct BlobVtable {
    QueryInterface query_interface;
    AddRef add_ref;
    AddRef release;
    BufferPointer buffer;
    BufferSize size;
};

HRESULT __stdcall blob_query_interface(Blob* self, const std::array<std::uint8_t, 16>* iid, void** out) {
    if (*iid == kIUnknown || *iid == kIdxcBlob) {
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return kNoInterface;
}

std::uint32_t __stdcall blob_add_ref(Blob*) { return 1; }

void* __stdcall blob_buffer(Blob* self) { return self->data; }

std::size_t __stdcall blob_size(Blob* self) { return self->length; }

const BlobVtable kBlobVtable = {blob_query_interface, blob_add_ref, blob_add_ref, blob_buffer, blob_size};

template <typename F>
F method(void* object, std::size_t slot) {
    static_assert(sizeof(F) == sizeof(void*), "COM slots hold one pointer");
    void* const* table = *static_cast<void* const* const*>(object);
    const void* entry = table[slot];
    F function;
    std::memcpy(&function, &entry, sizeof(function));
    return function;
}

void release_object(void* object) {
    if (object == nullptr) return;
    using Release = std::uint32_t(__stdcall*)(void*);
    method<Release>(object, 2)(object);
}

std::string hex32(std::uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size =
        static_cast<int>(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr));
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

void trim(std::string& text) {
    while (!text.empty() && text.back() == '\0') text.pop_back();
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last =
        std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    text = first < last ? std::string(first, last) : std::string{};
}

}  // namespace

Validator::~Validator() { release_object(object_); }

Validator::Validator(Validator&& other) noexcept : object_(other.object_) { other.object_ = nullptr; }

Validator& Validator::operator=(Validator&& other) noexcept {
    if (this != &other) {
        release_object(object_);
        object_ = other.object_;
        other.object_ = nullptr;
    }
    return *this;
}

std::optional<std::filesystem::path> Validator::default_path() {
    if (const char* configured = std::getenv("G6SHIM_DXIL")) return std::filesystem::path(configured);
    const std::filesystem::path kits = "C:\\Program Files (x86)\\Windows Kits\\10\\bin";
    std::error_code error;
    std::vector<std::filesystem::path> versions;
    for (std::filesystem::directory_iterator entry(kits, error), end; !error && entry != end;
         entry.increment(error)) {
        const std::filesystem::path candidate = entry->path() / "x64" / "dxil.dll";
        std::error_code exists_error;
        if (std::filesystem::exists(candidate, exists_error)) versions.push_back(entry->path());
    }
    if (versions.empty()) return std::nullopt;
    std::sort(versions.begin(), versions.end());
    return versions.back() / "x64" / "dxil.dll";
}

Validator Validator::load(const std::filesystem::path* path) {
    const std::wstring name = path == nullptr ? std::wstring(L"dxil.dll") : path->wstring();
    const HMODULE module = LoadLibraryW(name.c_str());
    if (module == nullptr) throw std::runtime_error("cannot load " + narrow(name));
    const FARPROC address = GetProcAddress(module, "DxcCreateInstance");
    if (address == nullptr) throw std::runtime_error("dxil.dll lacks DxcCreateInstance");
    using CreateInstance = HRESULT(__stdcall*)(const Guid*, const Guid*, void**);
    const auto create = reinterpret_cast<CreateInstance>(address);
    void* object = nullptr;
    const HRESULT result = create(&kClsidDxcValidator, &kIidIdxcValidator, &object);
    if (result < 0 || object == nullptr)
        throw std::runtime_error("DxcCreateInstance(validator) failed: " +
                                 hex32(static_cast<std::uint32_t>(result)));
    return Validator(object);
}

void Validator::validate(std::span<std::uint8_t> container) {
    Blob blob{reinterpret_cast<const void* const*>(&kBlobVtable), container.data(), container.size()};
    using Validate = HRESULT(__stdcall*)(void*, Blob*, std::uint32_t, void**);
    void* result = nullptr;
    const HRESULT status = method<Validate>(object_, 3)(object_, &blob, kInPlaceEdit, &result);
    if (status < 0 || result == nullptr)
        throw std::runtime_error("Validate failed: " + hex32(static_cast<std::uint32_t>(status)));
    HRESULT outcome = 0;
    using GetStatus = HRESULT(__stdcall*)(void*, HRESULT*);
    method<GetStatus>(result, 3)(result, &outcome);
    std::string text;
    if (outcome < 0) {
        void* errors = nullptr;
        using GetErrorBuffer = HRESULT(__stdcall*)(void*, void**);
        if (method<GetErrorBuffer>(result, 5)(result, &errors) >= 0 && errors != nullptr) {
            using Pointer = void*(__stdcall*)(void*);
            using Size = std::size_t(__stdcall*)(void*);
            const char* bytes = static_cast<const char*>(method<Pointer>(errors, 3)(errors));
            text.assign(bytes, method<Size>(errors, 4)(errors));
            trim(text);
            release_object(errors);
        }
        if (text.empty()) text = "rejected (" + hex32(static_cast<std::uint32_t>(outcome)) + ")";
    }
    release_object(result);
    if (outcome < 0) throw std::runtime_error(text);
}

}  // namespace g6shim::shader::emit
