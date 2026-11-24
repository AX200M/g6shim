// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/bvh/builder.hpp"
#include "d3d12_xs/bvh/reference.hpp"

#include <dxgi1_4.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace g6shim::d3d12_xs::bvh;

namespace {

constexpr std::uint32_t kArenaBytes = 8u << 20;
constexpr std::uint32_t kRayCount = 96;
constexpr std::uint32_t kLargeRays = 48;
constexpr std::uint32_t kRadixTriangles = 1104;
constexpr float kHitTolerance = 1e-3f;
constexpr float kInverseTolerance = 1e-4f;

struct Release {
    template <class T>
    void operator()(T* object) const {
        if (object != nullptr) object->Release();
    }
};

struct Session {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* arena = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
    std::uint8_t* upload_map = nullptr;
    const std::uint8_t* readback_map = nullptr;
    std::uint64_t host_base = 0;
    std::uint32_t cursor = kStructureAlignment;
    bool executed = false;

    ~Session() {
        if (readback_map != nullptr && readback != nullptr) readback->Unmap(0, nullptr);
        if (upload_map != nullptr && upload != nullptr) upload->Unmap(0, nullptr);
        Release{}(readback);
        Release{}(upload);
        Release{}(arena);
        Release{}(list);
        Release{}(allocator);
        Release{}(fence);
        Release{}(queue);
        Release{}(device);
    }

    bool open() {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT adapter_result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        factory->Release();
        if (FAILED(adapter_result)) return false;
        const HRESULT device_result =
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        adapter->Release();
        if (FAILED(device_result)) return false;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
            return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             IID_PPV_ARGS(&list))))
            return false;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        return create_buffers();
    }

    bool create_buffers() {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = buffer_desc(kArenaBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&arena))))
            return false;
        host_base = arena->GetGPUVirtualAddress();
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc = buffer_desc(kArenaBytes, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return false;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_map)))) return false;
        std::memset(upload_map, 0, kArenaBytes);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))))
            return false;
        return true;
    }

    static D3D12_RESOURCE_DESC buffer_desc(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    }

    AddressSpace space() const { return {host_base, kArenaBytes}; }

    std::uint32_t alloc(std::uint32_t bytes, std::uint32_t alignment) {
        const std::uint32_t at = (cursor + alignment - 1u) & ~(alignment - 1u);
        if (bytes > kArenaBytes || at > kArenaBytes - bytes) return 0;
        cursor = at + bytes;
        return at;
    }

    void write(std::uint32_t at, const void* data, std::uint32_t bytes) {
        std::memcpy(upload_map + at, data, bytes);
    }

    bool execute() {
        list->CopyBufferRegion(arena, 0, upload, 0, kArenaBytes);
        D3D12_RESOURCE_BARRIER to_uav{};
        to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav.Transition.pResource = arena;
        to_uav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_uav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_uav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_uav);
        return true;
    }

    void uav() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &barrier);
    }

    bool finish() {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = arena;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy);
        list->CopyBufferRegion(readback, 0, arena, 0, kArenaBytes);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence, 1))) return false;
        if (fence->GetCompletedValue() < 1) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event == nullptr) return false;
            if (FAILED(fence->SetEventOnCompletion(1, event))) {
                CloseHandle(event);
                return false;
            }
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) return false;
        readback_map = static_cast<const std::uint8_t*>(mapped);
        executed = true;
        return true;
    }

    const std::uint8_t* bytes(std::uint32_t at) const { return readback_map + at; }
};

struct TriRange {
    std::uint32_t geometry = 0;
    std::uint32_t primitive = 0;
};

struct Scene {
    std::vector<reference::Triangle> triangles;
    std::vector<TriRange> ranges;
    std::uint32_t va = 0;
    std::uint32_t layout_size = 0;
};

void add_triangle(Scene& scene, const float v[9], std::uint32_t geometry, std::uint32_t primitive) {
    reference::Triangle triangle{};
    for (int corner = 0; corner < 3; ++corner)
        for (int axis = 0; axis < 3; ++axis) triangle.vertices[corner][axis] = v[corner * 3 + axis];
    scene.triangles.push_back(triangle);
    scene.ranges.push_back({geometry, primitive});
}

void transform_point(const float matrix[12], const float in[3], float out[3]) {
    for (int row = 0; row < 3; ++row)
        out[row] = matrix[row * 4] * in[0] + matrix[row * 4 + 1] * in[1] + matrix[row * 4 + 2] * in[2] +
                   matrix[row * 4 + 3];
}

std::vector<reference::Triangle> transformed(const std::vector<reference::Triangle>& triangles, const float matrix[12]) {
    std::vector<reference::Triangle> out = triangles;
    for (reference::Triangle& triangle : out) {
        for (int corner = 0; corner < 3; ++corner) {
            float point[3] = {triangle.vertices[corner][0], triangle.vertices[corner][1], triangle.vertices[corner][2]};
            transform_point(matrix, point, triangle.vertices[corner]);
        }
    }
    return out;
}

std::uint32_t header_bytes(const std::uint8_t* data) {
    Header header{};
    std::memcpy(&header, data, sizeof(header));
    return (header.total_size_in_64b_nodes + 1u) * kUnit;
}

bool leaves_match(const std::uint8_t* gpu, const std::vector<reference::Triangle>& triangles, std::uint32_t geometry,
                  std::uint32_t flags, bool box16) {
    reference::BlasOptions options;
    options.geometry_index = geometry;
    options.geometry_flags = flags;
    options.box16 = box16;
    const std::vector<std::uint8_t> encoded = reference::build_blas(triangles, options);
    if (encoded.size() < kHeaderSize || header_bytes(gpu) < kHeaderSize) return false;
    Header gpu_header{};
    Header ref_header{};
    std::memcpy(&gpu_header, gpu, sizeof(gpu_header));
    std::memcpy(&ref_header, encoded.data(), sizeof(ref_header));
    if (ref_header.element_count != triangles.size()) return false;
    if (gpu_header.element_count < triangles.size()) return false;
    for (std::uint32_t element = 0; element < triangles.size(); ++element) {
        const std::uint8_t* left = gpu + gpu_header.elements_offset + element * kLeafSize;
        const std::uint8_t* right = encoded.data() + ref_header.elements_offset + element * kLeafSize;
        if (std::memcmp(left, right, kLeafSize) != 0) return false;
    }
    return true;
}

bool same_hit(const std::optional<reference::Hit>& gpu, const std::optional<reference::Hit>& brute,
              const std::vector<TriRange>& ranges, bool flat) {
    if (!gpu && !brute) return true;
    if (!gpu || !brute) return false;
    if (gpu->instance != brute->instance) return false;
    std::uint32_t gpu_flat = gpu->primitive;
    if (flat) {
        bool found = false;
        for (std::uint32_t index = 0; index < ranges.size(); ++index) {
            if (ranges[index].geometry == gpu->geometry && ranges[index].primitive == gpu->primitive) {
                gpu_flat = index;
                found = true;
                break;
            }
        }
        if (!found) return false;
    } else if (gpu->geometry != brute->geometry || gpu->primitive != brute->primitive) {
        return std::fabs(gpu->t - brute->t) <= kHitTolerance;
    }
    if (flat && gpu_flat != brute->primitive && std::fabs(gpu->t - brute->t) > kHitTolerance) return false;
    const float scale = std::fabs(brute->t) > 1.f ? std::fabs(brute->t) : 1.f;
    return std::fabs(gpu->t - brute->t) <= kHitTolerance * scale;
}

void check_rays(const std::uint8_t* structure, std::uint32_t va, const Scene& scene, std::uint32_t rays, std::uint32_t seed,
                bool flat) {
    const std::uint32_t bytes = header_bytes(structure);
    CHECK(bytes <= scene.layout_size);
    const auto problems = reference::validate({structure, bytes}, true);
    CHECK(problems.empty());
    reference::Memory memory;
    memory.place(va, {structure, bytes});
    reference::SceneInstance instance;
    instance.mask = 0xffu;
    instance.triangles = scene.triangles;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(-1.f, 1.f);
    for (std::uint32_t ray_index = 0; ray_index < rays; ++ray_index) {
        reference::Ray ray{};
        ray.origin[0] = unit(rng) * 8.f;
        ray.origin[1] = unit(rng) * 8.f;
        ray.origin[2] = unit(rng) * 8.f + 6.f;
        ray.direction[0] = unit(rng);
        ray.direction[1] = unit(rng);
        ray.direction[2] = -1.f;
        ray.t_max = 100.f;
        ray.mask = 0xffu;
        const auto gpu = reference::trace(memory, va, ray);
        const auto brute = reference::brute_force(std::span<const reference::SceneInstance>(&instance, 1), ray);
        CHECK(same_hit(gpu, brute, scene.ranges, flat));
    }
}

D3D12_RAYTRACING_GEOMETRY_DESC triangles_desc(std::uint64_t vertices, std::uint32_t stride, std::uint32_t vertex_count,
                                              DXGI_FORMAT vertex_format, std::uint64_t indices, DXGI_FORMAT index_format,
                                              std::uint32_t index_count, std::uint64_t transform) {
    D3D12_RAYTRACING_GEOMETRY_DESC desc{};
    desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    desc.Triangles.VertexBuffer.StartAddress = vertices;
    desc.Triangles.VertexBuffer.StrideInBytes = stride;
    desc.Triangles.VertexFormat = vertex_format;
    desc.Triangles.VertexCount = vertex_count;
    desc.Triangles.IndexBuffer = indices;
    desc.Triangles.IndexFormat = index_format;
    desc.Triangles.IndexCount = index_count;
    desc.Triangles.Transform3x4 = transform;
    return desc;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc(std::uint64_t dest, std::uint64_t scratch,
                                                             const D3D12_RAYTRACING_GEOMETRY_DESC* geometries,
                                                             UINT count, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData = dest;
    desc.ScratchAccelerationStructureData = scratch;
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.Flags = flags;
    desc.Inputs.NumDescs = count;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.pGeometryDescs = geometries;
    return desc;
}

void check_refused(Builder& builder, ID3D12GraphicsCommandList* list, const AddressSpace& space) {
    D3D12_RAYTRACING_GEOMETRY_DESC geometry =
        triangles_desc(256, 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = build_desc(256, 4096, &geometry, 1, {});
    desc.Inputs.Type = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE>(4);
    CHECK(builder.build(list, desc, space) == E_INVALIDARG);
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    CHECK(builder.build(list, desc, space) == E_INVALIDARG);
    desc.Inputs.Flags = {};
    desc.DestAccelerationStructureData = 128;
    CHECK(builder.build(list, desc, space) == E_INVALIDARG);
    desc.DestAccelerationStructureData = 256;
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    desc.Inputs.NumDescs = 1;
    CHECK(builder.build(list, desc, space) == E_NOTIMPL);
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.NumDescs = 1;
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    CHECK(builder.build(list, desc, space) == E_NOTIMPL);
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    CHECK(builder.build(list, desc, space) == E_NOTIMPL);
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_R8_UINT;
    geometry.Triangles.IndexCount = 3;
    CHECK(builder.build(list, desc, space) == E_NOTIMPL);
    CHECK(builder.copy(list, 256, 512, 256, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE, space) ==
          E_NOTIMPL);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{};
    post.DestBuffer = 256;
    post.InfoType = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE>(0x10);
    CHECK(builder.postbuild(list, post, 512, space) == E_INVALIDARG);
    post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
    CHECK(builder.postbuild(list, post, 512, space) == E_NOTIMPL);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geometry;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.IndexCount = 0;
    geometry.Triangles.VertexCount = 3;
    PrebuildSizes sizes{};
    CHECK(prebuild_sizes(inputs, &sizes) == S_OK);
    Layout layout{};
    CHECK(compute_layout(1, false, false, &layout));
    CHECK(sizes.result_max == layout.size && sizes.scratch == layout.scratch && sizes.update_scratch == layout.scratch);
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
    CHECK(prebuild_sizes(inputs, &sizes) == S_OK);
    CHECK(compute_layout(1, false, true, &layout));
    CHECK(sizes.result_max == layout.size && sizes.scratch == layout.scratch);
}

std::uint32_t place_floats(Session& session, const std::vector<float>& values) {
    const auto bytes = static_cast<std::uint32_t>(values.size() * sizeof(float));
    const std::uint32_t at = session.alloc(bytes, 16);
    session.write(at, values.data(), bytes);
    return at;
}

std::uint32_t place_u16(Session& session, const std::vector<std::uint16_t>& values) {
    const auto bytes = static_cast<std::uint32_t>(values.size() * sizeof(std::uint16_t));
    const std::uint32_t at = session.alloc(bytes == 0 ? 4 : bytes, 4);
    if (bytes != 0) session.write(at, values.data(), bytes);
    return at;
}

std::uint32_t place_u32(Session& session, const std::vector<std::uint32_t>& values) {
    const auto bytes = static_cast<std::uint32_t>(values.size() * sizeof(std::uint32_t));
    const std::uint32_t at = session.alloc(bytes, 16);
    session.write(at, values.data(), bytes);
    return at;
}

Scene soup(std::uint32_t count, std::uint32_t seed) {
    Scene scene;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> spread(-4.f, 4.f);
    std::uniform_real_distribution<float> jitter(-0.6f, 0.6f);
    for (std::uint32_t triangle = 0; triangle < count; ++triangle) {
        const float centre[3] = {spread(rng), spread(rng), spread(rng)};
        float values[9];
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) values[corner * 3 + axis] = centre[axis] + jitter(rng);
        add_triangle(scene, values, 0, triangle);
    }
    return scene;
}

bool record_blas(Session& session, Builder& builder, Scene& scene, std::uint32_t scratch,
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags, const D3D12_RAYTRACING_GEOMETRY_DESC* geometries,
                 UINT geometry_count, std::uint64_t source) {
    Layout layout{};
    const bool box16 = (flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY) != 0;
    std::uint32_t count = 0;
    for (UINT index = 0; index < geometry_count; ++index) {
        const auto& triangles = geometries[index].Triangles;
        count += triangles.IndexFormat == DXGI_FORMAT_UNKNOWN ? triangles.VertexCount / 3u : triangles.IndexCount / 3u;
    }
    if (!compute_layout(count, false, box16, &layout)) return false;
    const std::uint32_t dest = session.alloc(layout.size, kStructureAlignment);
    if (dest == 0) return false;
    auto desc = build_desc(dest, scratch, geometries, geometry_count, flags);
    desc.SourceAccelerationStructureData = source;
    if (FAILED(builder.build(session.list, desc, session.space()))) return false;
    session.uav();
    scene.va = dest;
    scene.layout_size = layout.size;
    return true;
}

bool has_tri1(const std::uint8_t* data) {
    Header header{};
    std::memcpy(&header, data, sizeof(header));
    std::vector<std::uint32_t> stack{header.root};
    for (std::size_t steps = 0; !stack.empty() && steps < 100000; ++steps) {
        const std::uint32_t encoded = stack.back();
        stack.pop_back();
        if (encoded == kInvalid) continue;
        if (pointer_type(encoded) == kTri1) return true;
        if (pointer_type(encoded) != kBox16 && pointer_type(encoded) != kBox32) continue;
        std::uint32_t children[4];
        std::memcpy(children, data + pointer_offset(encoded), sizeof(children));
        for (std::uint32_t child : children) stack.push_back(child);
    }
    return false;
}

}  // namespace

TEST(bvh_builder_matches_reference) {
    const auto started = std::chrono::steady_clock::now();
    Session session;
    if (!session.open()) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    auto builder = Builder::create(session.device);
    CHECK(builder != nullptr);
    if (!builder) return;
    check_refused(*builder, session.list, session.space());
    CHECK(session.execute());

    Layout scratch_layout{};
    CHECK(compute_layout(kRadixTriangles, false, false, &scratch_layout));
    const std::uint32_t scratch = session.alloc(static_cast<std::uint32_t>(scratch_layout.scratch), kStructureAlignment);
    CHECK(scratch != 0);

    Scene one = soup(1, 1);
    std::vector<float> one_vertices;
    for (const auto& triangle : one.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) one_vertices.push_back(triangle.vertices[corner][axis]);
    const std::uint32_t one_va = place_floats(session, one_vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC one_geo =
        triangles_desc(one_va, 12, 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, one, scratch, {}, &one_geo, 1, 0));

    Scene five = soup(5, 2);
    std::vector<float> five_vertices;
    for (const auto& triangle : five.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) five_vertices.push_back(triangle.vertices[corner][axis]);
    const std::uint32_t five_va = place_floats(session, five_vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC five_geo =
        triangles_desc(five_va, 12, 15, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, five, scratch, {}, &five_geo, 1, 0));

    Scene random = soup(kRadixTriangles, 7);
    std::vector<float> random_vertices;
    random_vertices.reserve(kRadixTriangles * 9u);
    for (const auto& triangle : random.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) random_vertices.push_back(triangle.vertices[corner][axis]);
    const std::uint32_t random_va = place_floats(session, random_vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC random_geo = triangles_desc(
        random_va, 12, kRadixTriangles * 3u, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, random, scratch, {}, &random_geo, 1, 0));

    Scene grid;
    constexpr std::uint32_t kGrid = 4;
    std::vector<float> grid_vertices;
    std::vector<std::uint16_t> grid_indices;
    for (std::uint32_t y = 0; y <= kGrid; ++y) {
        for (std::uint32_t x = 0; x <= kGrid; ++x) {
            grid_vertices.push_back(static_cast<float>(x) - kGrid * 0.5f);
            grid_vertices.push_back(static_cast<float>(y) - kGrid * 0.5f);
            grid_vertices.push_back(0.25f);
            grid_vertices.push_back(9.f);
        }
    }
    for (std::uint32_t y = 0; y < kGrid; ++y) {
        for (std::uint32_t x = 0; x < kGrid; ++x) {
            const std::uint32_t a = y * (kGrid + 1) + x;
            const std::uint16_t q[6] = {static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(a + 1),
                                        static_cast<std::uint16_t>(a + kGrid + 2), static_cast<std::uint16_t>(a),
                                        static_cast<std::uint16_t>(a + kGrid + 2),
                                        static_cast<std::uint16_t>(a + kGrid + 1)};
            grid_indices.insert(grid_indices.end(), q, q + 6);
            float tri0[9] = {grid_vertices[q[0] * 4], grid_vertices[q[0] * 4 + 1], grid_vertices[q[0] * 4 + 2],
                             grid_vertices[q[1] * 4], grid_vertices[q[1] * 4 + 1], grid_vertices[q[1] * 4 + 2],
                             grid_vertices[q[2] * 4], grid_vertices[q[2] * 4 + 1], grid_vertices[q[2] * 4 + 2]};
            float tri1[9] = {grid_vertices[q[3] * 4], grid_vertices[q[3] * 4 + 1], grid_vertices[q[3] * 4 + 2],
                             grid_vertices[q[4] * 4], grid_vertices[q[4] * 4 + 1], grid_vertices[q[4] * 4 + 2],
                             grid_vertices[q[5] * 4], grid_vertices[q[5] * 4 + 1], grid_vertices[q[5] * 4 + 2]};
            add_triangle(grid, tri0, 0, static_cast<std::uint32_t>(grid.triangles.size()));
            add_triangle(grid, tri1, 0, static_cast<std::uint32_t>(grid.triangles.size()));
        }
    }
    const std::uint32_t grid_v = place_floats(session, grid_vertices);
    const std::uint32_t grid_i = place_u16(session, grid_indices);
    D3D12_RAYTRACING_GEOMETRY_DESC grid_geo =
        triangles_desc(grid_v, 16, (kGrid + 1) * (kGrid + 1), DXGI_FORMAT_R32G32B32_FLOAT, grid_i, DXGI_FORMAT_R16_UINT,
                       static_cast<std::uint32_t>(grid_indices.size()), 0);
    CHECK(record_blas(session, *builder, grid, scratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
                      &grid_geo, 1, 0));
    Scene paired_grid = grid;
    CHECK(record_blas(session, *builder, paired_grid, scratch, {}, &grid_geo, 1, 0));

    Scene xy;
    std::vector<float> xy_vertices;
    std::vector<std::uint32_t> xy_indices;
    constexpr std::uint32_t kXy = 3;
    for (std::uint32_t y = 0; y <= kXy; ++y) {
        for (std::uint32_t x = 0; x <= kXy; ++x) {
            xy_vertices.push_back(static_cast<float>(x));
            xy_vertices.push_back(static_cast<float>(y));
        }
    }
    for (std::uint32_t y = 0; y < kXy; ++y) {
        for (std::uint32_t x = 0; x < kXy; ++x) {
            const std::uint32_t a = y * (kXy + 1) + x;
            const std::uint32_t q[6] = {a, a + 1, a + kXy + 2, a, a + kXy + 2, a + kXy + 1};
            xy_indices.insert(xy_indices.end(), q, q + 6);
            float tri0[9] = {xy_vertices[q[0] * 2], xy_vertices[q[0] * 2 + 1], 0, xy_vertices[q[1] * 2],
                             xy_vertices[q[1] * 2 + 1], 0, xy_vertices[q[2] * 2], xy_vertices[q[2] * 2 + 1], 0};
            float tri1[9] = {xy_vertices[q[3] * 2], xy_vertices[q[3] * 2 + 1], 0, xy_vertices[q[4] * 2],
                             xy_vertices[q[4] * 2 + 1], 0, xy_vertices[q[5] * 2], xy_vertices[q[5] * 2 + 1], 0};
            add_triangle(xy, tri0, 0, static_cast<std::uint32_t>(xy.triangles.size()));
            add_triangle(xy, tri1, 0, static_cast<std::uint32_t>(xy.triangles.size()));
        }
    }
    const std::uint32_t xy_v = place_floats(session, xy_vertices);
    const std::uint32_t xy_i = place_u32(session, xy_indices);
    D3D12_RAYTRACING_GEOMETRY_DESC xy_geo =
        triangles_desc(xy_v, 8, (kXy + 1) * (kXy + 1), DXGI_FORMAT_R32G32_FLOAT, xy_i, DXGI_FORMAT_R32_UINT,
                       static_cast<std::uint32_t>(xy_indices.size()), 0);
    CHECK(record_blas(session, *builder, xy, scratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
                      &xy_geo, 1, 0));

    const float matrix[12] = {0, 2, 0, 1, 0, 0, 1, -2, 2, 0, 0, 3};
    Scene moved = soup(6, 9);
    const auto moved_world = transformed(moved.triangles, matrix);
    std::vector<float> moved_vertices;
    for (const auto& triangle : moved.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) moved_vertices.push_back(triangle.vertices[corner][axis]);
    const std::uint32_t moved_va = place_floats(session, moved_vertices);
    const std::uint32_t matrix_va = place_floats(session, std::vector<float>(matrix, matrix + 12));
    D3D12_RAYTRACING_GEOMETRY_DESC moved_geo =
        triangles_desc(moved_va, 12, 18, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, matrix_va);
    Scene moved_scene = moved;
    moved_scene.triangles = moved_world;
    CHECK(record_blas(session, *builder, moved_scene, scratch, {}, &moved_geo, 1, 0));

    Scene multi_a = soup(4, 11);
    Scene multi_b = soup(3, 12);
    multi_b.triangles[1].vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> multi_av;
    std::vector<float> multi_bv;
    for (const auto& triangle : multi_a.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) multi_av.push_back(triangle.vertices[corner][axis]);
    for (const auto& triangle : multi_b.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) multi_bv.push_back(triangle.vertices[corner][axis]);
    const std::uint32_t multi_a_va = place_floats(session, multi_av);
    const std::uint32_t multi_b_va = place_floats(session, multi_bv);
    D3D12_RAYTRACING_GEOMETRY_DESC multi_geo[2] = {
        triangles_desc(multi_a_va, 12, 12, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0),
        triangles_desc(multi_b_va, 12, 9, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0)};
    Scene multi;
    for (std::uint32_t i = 0; i < multi_a.triangles.size(); ++i) {
        multi.triangles.push_back(multi_a.triangles[i]);
        multi.ranges.push_back({0, i});
    }
    for (std::uint32_t i = 0; i < multi_b.triangles.size(); ++i) {
        multi.triangles.push_back(multi_b.triangles[i]);
        multi.ranges.push_back({1, i});
    }
    CHECK(record_blas(session, *builder, multi, scratch, {}, multi_geo, 2, 0));

    Scene empty;
    CHECK(record_blas(session, *builder, empty, scratch, {}, nullptr, 0, 0));

    Scene extremes;
    const float odd[][9] = {
        {70000.f, 1.f, 2.f, 70001.f, 3.f, 2.5f, 69999.f, 2.f, 1.5f},
        {-65520.f, -65504.f, 65504.f, -65519.f, -65500.f, 65505.f, -65000.f, -65503.f, 65490.f},
        {3e-5f, -2e-6f, 1e-7f, 6e-5f, 4e-5f, -1e-9f, -3e-5f, 6.103515625e-5f, 0.f},
        {-0.f, 1e-30f, -1e-30f, 2.f, 0.5f, 0.25f, 1.f, -0.125f, 0.f},
        {1e30f, -1e30f, 5.f, 1e30f, -1e30f, 6.f, -1e30f, 1e30f, 5.5f},
        {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f},
    };
    std::vector<float> odd_vertices;
    for (const auto& triangle : odd) {
        add_triangle(extremes, triangle, 0, static_cast<std::uint32_t>(extremes.triangles.size()));
        odd_vertices.insert(odd_vertices.end(), triangle, triangle + 9);
    }
    const std::uint32_t odd_va = place_floats(session, odd_vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC odd_geo = triangles_desc(
        odd_va, 12, static_cast<std::uint32_t>(odd_vertices.size() / 3), DXGI_FORMAT_R32G32B32_FLOAT, 0,
        DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, extremes, scratch,
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY, &odd_geo, 1, 0));

    const float pair_values[18] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1};
    Scene pair_scene;
    add_triangle(pair_scene, pair_values, 0, 0);
    add_triangle(pair_scene, pair_values + 9, 0, 1);
    const std::uint32_t pair_va = place_floats(session, std::vector<float>(pair_values, pair_values + 18));
    D3D12_RAYTRACING_GEOMETRY_DESC pair_geo =
        triangles_desc(pair_va, 12, 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, pair_scene, scratch, {}, &pair_geo, 1, 0));

    Scene refit = soup(8, 21);
    std::vector<float> refit_vertices;
    std::vector<float> refit_moved;
    for (const auto& triangle : refit.triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            for (int axis = 0; axis < 3; ++axis) refit_vertices.push_back(triangle.vertices[corner][axis]);
        }
    }
    Scene refit_after = refit;
    for (auto& triangle : refit_after.triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            triangle.vertices[corner][0] = triangle.vertices[corner][0] * 1.25f + 0.2f;
            triangle.vertices[corner][1] = triangle.vertices[corner][1] * 0.8f - 0.1f;
            for (int axis = 0; axis < 3; ++axis) refit_moved.push_back(triangle.vertices[corner][axis]);
        }
    }
    const std::uint32_t refit_va = place_floats(session, refit_vertices);
    const std::uint32_t refit_moved_va = place_floats(session, refit_moved);
    D3D12_RAYTRACING_GEOMETRY_DESC refit_geo =
        triangles_desc(refit_va, 12, 24, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, refit, scratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
                      &refit_geo, 1, 0));
    D3D12_RAYTRACING_GEOMETRY_DESC refit_geo_moved =
        triangles_desc(refit_moved_va, 12, 24, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    Scene refit_in_place = refit_after;
    Layout refit_layout{};
    CHECK(compute_layout(8, false, false, &refit_layout));
    {
        auto desc = build_desc(refit.va, scratch, &refit_geo_moved, 1,
                               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        desc.SourceAccelerationStructureData = refit.va;
        CHECK(builder->build(session.list, desc, session.space()) == S_OK);
        session.uav();
        refit_in_place.va = refit.va;
        refit_in_place.layout_size = refit.layout_size;
    }
    Scene other = soup(8, 22);
    std::vector<float> other_vertices;
    std::vector<float> other_moved_vertices;
    for (const auto& triangle : other.triangles)
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis) other_vertices.push_back(triangle.vertices[corner][axis]);
    Scene other_after = other;
    for (auto& triangle : other_after.triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            triangle.vertices[corner][0] += 0.4f;
            triangle.vertices[corner][2] -= 0.3f;
            for (int axis = 0; axis < 3; ++axis) other_moved_vertices.push_back(triangle.vertices[corner][axis]);
        }
    }
    const std::uint32_t other_va = place_floats(session, other_vertices);
    const std::uint32_t other_moved_va = place_floats(session, other_moved_vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC other_geo =
        triangles_desc(other_va, 12, 24, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    CHECK(record_blas(session, *builder, other, scratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE,
                      &other_geo, 1, 0));
    D3D12_RAYTRACING_GEOMETRY_DESC other_moved_geo =
        triangles_desc(other_moved_va, 12, 24, DXGI_FORMAT_R32G32B32_FLOAT, 0, DXGI_FORMAT_UNKNOWN, 0, 0);
    Scene other_dest = other_after;
    {
        const std::uint32_t dest = session.alloc(other.layout_size, kStructureAlignment);
        auto desc = build_desc(dest, scratch, &other_moved_geo, 1,
                               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
        desc.SourceAccelerationStructureData = other.va;
        CHECK(builder->build(session.list, desc, session.space()) == S_OK);
        session.uav();
        other_dest.va = dest;
        other_dest.layout_size = other.layout_size;
    }

    const std::uint32_t info = session.alloc(16, 8);
    const std::uint32_t clone = session.alloc(five.layout_size, kStructureAlignment);
    CHECK(builder->copy(session.list, clone, five.va, five.layout_size,
                        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE, session.space()) == S_OK);
    session.uav();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{};
    post.DestBuffer = info;
    post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    CHECK(builder->postbuild(session.list, post, five.va, session.space()) == S_OK);
    session.uav();
    post.DestBuffer = info + 8;
    post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
    CHECK(builder->postbuild(session.list, post, clone, session.space()) == S_OK);
    session.uav();
    const std::uint32_t packed = session.alloc(five.layout_size + 4u, kStructureAlignment);
    std::memset(session.upload_map + packed, 0xA5, five.layout_size + 4u);
    CHECK(builder->copy(session.list, packed, five.va, five.layout_size,
                        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT, session.space()) == S_OK);
    session.uav();

    const float turned[12] = {0.8f, 0, 0.6f, 4, 0, 1, 0, -3, -0.6f, 0, 0.8f, 2};
    const float scaled[12] = {2, 0, 0, -9, 0, 0.5f, 0, 2, 0, 0, 1.5f, 1};
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    struct InstDesc {
        float transform[12];
        std::uint32_t id_mask;
        std::uint32_t contribution;
        std::uint64_t blas;
    };
    InstDesc instances[4]{};
    std::memcpy(instances[0].transform, identity, sizeof(identity));
    instances[0].id_mask = 0xff000000u | 10u;
    instances[0].blas = five.va;
    std::memcpy(instances[1].transform, turned, sizeof(turned));
    instances[1].id_mask = 0x01000000u | 11u;
    instances[1].contribution = 3;
    instances[1].blas = grid.va;
    std::memcpy(instances[2].transform, scaled, sizeof(scaled));
    instances[2].id_mask = 12u;
    instances[2].blas = five.va;
    std::memcpy(instances[3].transform, identity, sizeof(identity));
    instances[3].id_mask = 0xff000000u | 13u;
    instances[3].blas = 0;
    const std::uint32_t instance_va = session.alloc(sizeof(instances), 16);
    session.write(instance_va, instances, sizeof(instances));
    Layout tlas_layout{};
    CHECK(compute_layout(4, true, false, &tlas_layout));
    const std::uint32_t tlas = session.alloc(tlas_layout.size, kStructureAlignment);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_desc{};
    tlas_desc.DestAccelerationStructureData = tlas;
    tlas_desc.ScratchAccelerationStructureData = scratch;
    tlas_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_desc.Inputs.NumDescs = 4;
    tlas_desc.Inputs.InstanceDescs = instance_va;
    CHECK(builder->build(session.list, tlas_desc, session.space()) == S_OK);
    session.uav();

    CHECK(session.finish());
    if (!session.executed) return;

    CHECK(leaves_match(session.bytes(one.va), one.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(five.va), five.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(random.va), random.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(grid.va), grid.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(xy.va), xy.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(moved_scene.va), moved_world, 0, 1, false));
    CHECK(leaves_match(session.bytes(multi.va), multi_a.triangles, 0, 1, false));
    Header multi_header{};
    std::memcpy(&multi_header, session.bytes(multi.va), sizeof(multi_header));
    {
        reference::BlasOptions options;
        options.geometry_index = 1;
        options.geometry_flags = 1;
        const auto encoded = reference::build_blas(multi_b.triangles, options);
        Header ref_header{};
        std::memcpy(&ref_header, encoded.data(), sizeof(ref_header));
        for (std::uint32_t element = 0; element < multi_b.triangles.size(); ++element) {
            CHECK(std::memcmp(session.bytes(multi.va) + multi_header.elements_offset + (4u + element) * kLeafSize,
                              encoded.data() + ref_header.elements_offset + element * kLeafSize, kLeafSize) == 0);
        }
    }
    CHECK(leaves_match(session.bytes(extremes.va), extremes.triangles, 0, 1, true));
    const auto extreme_bytes = header_bytes(session.bytes(extremes.va));
    CHECK(reference::box16_rounding({session.bytes(extremes.va), extreme_bytes}).empty());
    CHECK(leaves_match(session.bytes(refit_in_place.va), refit_after.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(other.va), other.triangles, 0, 1, false));
    CHECK(leaves_match(session.bytes(other_dest.va), other_after.triangles, 0, 1, false));

    check_rays(session.bytes(one.va), one.va, one, kRayCount, 3, false);
    check_rays(session.bytes(five.va), five.va, five, kRayCount, 4, false);
    check_rays(session.bytes(random.va), random.va, random, kLargeRays, 5, false);
    check_rays(session.bytes(grid.va), grid.va, grid, kRayCount, 6, false);
    check_rays(session.bytes(paired_grid.va), paired_grid.va, paired_grid, kRayCount, 16, false);
    CHECK(has_tri1(session.bytes(paired_grid.va)));
    check_rays(session.bytes(xy.va), xy.va, xy, kRayCount, 7, false);
    check_rays(session.bytes(moved_scene.va), moved_scene.va, moved_scene, kRayCount, 8, false);
    check_rays(session.bytes(multi.va), multi.va, multi, kRayCount, 9, true);
    {
        const std::uint32_t bytes = header_bytes(session.bytes(empty.va));
        CHECK(reference::validate({session.bytes(empty.va), bytes}, true).empty());
        reference::Memory memory;
        memory.place(empty.va, {session.bytes(empty.va), bytes});
        reference::Ray ray{};
        ray.direction[2] = -1.f;
        ray.t_max = 10.f;
        ray.mask = 0xffu;
        CHECK(!reference::trace(memory, empty.va, ray));
    }
    check_rays(session.bytes(extremes.va), extremes.va, extremes, kRayCount, 10, false);
    check_rays(session.bytes(pair_scene.va), pair_scene.va, pair_scene, kRayCount, 11, false);
    CHECK(has_tri1(session.bytes(pair_scene.va)));
    check_rays(session.bytes(refit_in_place.va), refit_in_place.va, refit_in_place, kRayCount, 12, false);
    check_rays(session.bytes(other.va), other.va, other, kRayCount, 13, false);
    check_rays(session.bytes(other_dest.va), other_dest.va, other_dest, kRayCount, 14, false);

    const std::uint32_t five_bytes = header_bytes(session.bytes(five.va));
    CHECK(std::memcmp(session.bytes(clone), session.bytes(five.va), five_bytes) == 0);
    std::uint64_t compacted = 0;
    std::uint64_t current = 0;
    std::memcpy(&compacted, session.bytes(info), 8);
    std::memcpy(&current, session.bytes(info + 8), 8);
    CHECK(compacted == five_bytes && current == five_bytes);
    CHECK(std::memcmp(session.bytes(packed), session.bytes(five.va), five_bytes) == 0);
    CHECK(session.bytes(packed)[five_bytes] == 0xA5);

    Header five_header{};
    std::memcpy(&five_header, session.bytes(five.va), sizeof(five_header));
    Header grid_header{};
    std::memcpy(&grid_header, session.bytes(grid.va), sizeof(grid_header));
    const std::uint32_t tlas_bytes = header_bytes(session.bytes(tlas));
    CHECK(reference::validate({session.bytes(tlas), tlas_bytes}, false).empty());
    Header tlas_header{};
    std::memcpy(&tlas_header, session.bytes(tlas), sizeof(tlas_header));
    for (std::uint32_t index = 0; index < 4; ++index) {
        InstanceRecord record{};
        std::memcpy(&record, session.bytes(tlas) + tlas_header.elements_offset + index * kInstanceSize, sizeof(record));
        CHECK(std::memcmp(record.object_to_world, instances[index].transform, sizeof(instances[index].transform)) == 0);
        CHECK(record.id_and_mask == instances[index].id_mask);
        CHECK(record.contribution_and_flags == instances[index].contribution);
        CHECK(record.blas_address_256 == static_cast<std::uint32_t>(instances[index].blas >> 8));
        CHECK(record.index == index);
        float inverse[12];
        const bool inverted = reference::invert_affine(instances[index].transform, inverse);
        CHECK(inverted);
        if (inverted) {
            for (int component = 0; component < 12; ++component)
                CHECK(std::fabs(record.world_to_object[component] - inverse[component]) <= kInverseTolerance);
        }
        const std::uint32_t blas_root = instances[index].blas == five.va ? five_header.root : grid_header.root;
        if (instances[index].blas != 0) CHECK((record.root_and_flags & ~kInstanceFlagsMask) == blas_root);
    }
    reference::Memory memory;
    memory.place(tlas, {session.bytes(tlas), tlas_bytes});
    memory.place(five.va, {session.bytes(five.va), five_bytes});
    memory.place(grid.va, {session.bytes(grid.va), header_bytes(session.bytes(grid.va))});
    reference::SceneInstance scene_instances[4];
    scene_instances[0].transform = std::array<float, 12>{};
    std::memcpy(scene_instances[0].transform->data(), identity, sizeof(identity));
    scene_instances[0].mask = 0xffu;
    scene_instances[0].triangles = five.triangles;
    scene_instances[1].transform = std::array<float, 12>{};
    std::memcpy(scene_instances[1].transform->data(), turned, sizeof(turned));
    scene_instances[1].mask = 1u;
    scene_instances[1].triangles = grid.triangles;
    scene_instances[2].transform = std::array<float, 12>{};
    std::memcpy(scene_instances[2].transform->data(), scaled, sizeof(scaled));
    scene_instances[2].mask = 0;
    scene_instances[2].triangles = five.triangles;
    scene_instances[3].mask = 0xffu;
    std::mt19937 rng(15);
    std::uniform_real_distribution<float> unit(-1.f, 1.f);
    for (std::uint32_t ray_index = 0; ray_index < kRayCount; ++ray_index) {
        reference::Ray ray{};
        ray.origin[0] = unit(rng) * 12.f;
        ray.origin[1] = unit(rng) * 12.f;
        ray.origin[2] = 8.f + unit(rng);
        ray.direction[0] = unit(rng) * 0.2f;
        ray.direction[1] = unit(rng) * 0.2f;
        ray.direction[2] = -1.f;
        ray.t_max = 40.f;
        ray.mask = 0xffu;
        const auto gpu = reference::trace(memory, tlas, ray);
        const auto brute = reference::brute_force(scene_instances, ray);
        CHECK(same_hit(gpu, brute, {}, false));
    }

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("  bvh builder %.1f ms\n", ms);
}
