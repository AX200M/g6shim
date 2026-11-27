// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g6shim::upscaler {

using ReturnCode = std::uint32_t;
using Context = void*;
using StructType = std::uint64_t;

inline constexpr ReturnCode kOk = 0;
inline constexpr ReturnCode kError = 1;
inline constexpr ReturnCode kUnknownType = 2;
inline constexpr ReturnCode kRuntimeError = 3;
inline constexpr ReturnCode kNoProvider = 4;
inline constexpr ReturnCode kParameter = 6;

inline constexpr StructType kEffectMask = 0x00ff0000u;
inline constexpr StructType kBackendMask = 0xff000000u;
inline constexpr StructType kBackendDx12 = 0x00000000u;
inline constexpr StructType kBackendConsole = 0x02000000u;
inline constexpr StructType kEffectUpscale = 0x00010000u;

inline constexpr StructType kBackendDevice = 0x02u;
inline constexpr StructType kBackendAllocation = 0x03u;
inline constexpr StructType kOverrideVersion = 5u;
inline constexpr StructType kQueryVersions = 4u;
inline constexpr StructType kQueryProviderVersion = 6u;
inline constexpr StructType kCreateUpscale = kEffectUpscale | 0x00u;
inline constexpr StructType kDispatchUpscale = kEffectUpscale | 0x01u;
inline constexpr StructType kDispatchReactiveMask = kEffectUpscale | 0x06u;
inline constexpr StructType kQueryMemoryUsage = kEffectUpscale | 0x09u;
inline constexpr StructType kCreateUpscaleVersion = kEffectUpscale | 0x0bu;

struct Header {
    StructType type;
    Header* next;
};

struct AllocationCallbacks {
    void* user;
    void* (*alloc)(void* user, std::uint64_t size);
    void (*dealloc)(void* user, void* memory);
};

struct BackendDevice {
    Header header;
    void* device;
};

struct Dimensions {
    std::uint32_t width;
    std::uint32_t height;
};

struct FloatPair {
    float x;
    float y;
};

struct CreateUpscale {
    Header header;
    std::uint32_t flags;
    Dimensions max_render;
    Dimensions max_upscale;
    void (*message)(std::uint32_t type, const wchar_t* text);
};

struct CreateUpscaleVersion {
    Header header;
    std::uint32_t version;
};

struct ResourceDescription {
    std::uint32_t type;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t mip_count;
    std::uint32_t flags;
    std::uint32_t usage;
};

struct Resource {
    void* resource;
    ResourceDescription description;
    std::uint32_t state;
};

struct DispatchUpscale {
    Header header;
    void* command_list;
    Resource color;
    Resource depth;
    Resource motion_vectors;
    Resource exposure;
    Resource reactive;
    Resource transparency;
    Resource output;
    FloatPair jitter;
    FloatPair motion_vector_scale;
    Dimensions render_size;
    Dimensions upscale_size;
    bool sharpen;
    float sharpness;
    float frame_time_delta;
    float pre_exposure;
    bool reset;
    float camera_near;
    float camera_far;
    float camera_fov;
    float meters_per_unit;
    std::uint32_t flags;
};

struct DispatchReactiveMask {
    Header header;
    void* command_list;
    Resource opaque_color;
    Resource color;
    Resource out_reactive;
    Dimensions render_size;
    float scale;
    float cutoff;
    float binary;
    std::uint32_t flags;
};

struct QueryVersions {
    Header header;
    std::uint64_t create_type;
    void* device;
    std::uint64_t* count;
    std::uint64_t* ids;
    const char** names;
};

struct QueryProviderVersion {
    Header header;
    std::uint64_t id;
    const char* name;
};

struct QueryMemoryUsage {
    Header header;
    void* device;
    Dimensions max_render;
    Dimensions max_upscale;
    std::uint32_t flags;
    void* usage;
};

enum class State : std::uint32_t {
    kCommon = 1u << 0,
    kUnorderedAccess = 1u << 1,
    kComputeRead = 1u << 2,
    kPixelRead = 1u << 3,
    kCopySource = 1u << 4,
    kCopyDest = 1u << 5,
    kIndirectArgument = 1u << 6,
    kPresent = 1u << 7,
    kRenderTarget = 1u << 8,
    kDepthAttachment = 1u << 9,
};

enum class ResourceType : std::uint32_t { kBuffer, kTexture1D, kTexture2D, kTextureCube, kTexture3D };

inline constexpr std::uint32_t kUsageUnorderedAccess = 1u << 1;
inline constexpr std::uint32_t kUsageDepthTarget = 1u << 2;

enum class SurfaceFormat : std::uint32_t {
    kUnknown,
    kR32G32B32A32Typeless,
    kR32G32B32A32Uint,
    kR32G32B32A32Float,
    kR16G16B16A16Float,
    kR32G32B32Float,
    kR32G32Float,
    kR8Uint,
    kR32Uint,
    kR8G8B8A8Typeless,
    kR8G8B8A8Unorm,
    kR8G8B8A8Snorm,
    kR8G8B8A8Srgb,
    kB8G8R8A8Typeless,
    kB8G8R8A8Unorm,
    kB8G8R8A8Srgb,
    kR11G11B10Float,
    kR10G10B10A2Unorm,
    kR16G16Float,
    kR16G16Uint,
    kR16G16Sint,
    kR16Float,
    kR16Uint,
    kR16Unorm,
    kR16Snorm,
    kR8Unorm,
    kR8G8Unorm,
    kR8G8Uint,
    kR32Float,
    kR9G9B9E5SharedExp,
    kR16G16B16A16Typeless,
    kR32G32Typeless,
    kR10G10B10A2Typeless,
    kR16G16Typeless,
    kR16Typeless,
    kR8Typeless,
    kR8G8Typeless,
    kR32Typeless,
    kR32G32Uint,
    kR8Snorm,
};

using CreateContextFn = ReturnCode (*)(Context* context, Header* desc, const AllocationCallbacks* memory);
using DestroyContextFn = ReturnCode (*)(Context* context, const AllocationCallbacks* memory);
using ConfigureFn = ReturnCode (*)(Context* context, const Header* desc);
using QueryFn = ReturnCode (*)(Context* context, Header* desc);
using DispatchFn = ReturnCode (*)(Context* context, const Header* desc);

}  // namespace g6shim::upscaler
