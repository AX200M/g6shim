// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/bvh/builder.hpp"
#include "d3d12_xs/bvh/format.hpp"
#include "d3d12_xs/bvh/reference.hpp"
#include "d3d12_xs/list.hpp"
#include "d3d12_xs/queue.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <cstring>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

constexpr std::size_t kBuildSlot = 117;
constexpr std::size_t kEmitSlot = 118;
constexpr std::size_t kCopySlot = 119;
constexpr std::size_t kBatchedSlot = 122;
constexpr std::size_t kNewerShift = 2;
constexpr std::uint32_t kRayCount = 8;

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

struct BatchedBuild {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc;
    std::uint32_t postbuild_count;
    std::uint32_t reserved;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuild;
    std::uint64_t stream_bytes;
    const void* stream;
};

std::uint32_t header_bytes(const std::uint8_t* data) {
    bvh::Header header{};
    std::memcpy(&header, data, sizeof header);
    return (header.total_size_in_64b_nodes + 1u) * bvh::kUnit;
}

D3D12_RAYTRACING_GEOMETRY_DESC triangle_desc(std::uint64_t vertices) {
    D3D12_RAYTRACING_GEOMETRY_DESC desc{};
    desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    desc.Triangles.VertexCount = 3;
    desc.Triangles.VertexBuffer.StartAddress = vertices;
    desc.Triangles.VertexBuffer.StrideInBytes = 12;
    desc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    return desc;
}

bool same_hit(const std::optional<bvh::reference::Hit>& left, const std::optional<bvh::reference::Hit>& right) {
    if (!left && !right) return true;
    if (!left || !right) return false;
    if (left->instance != right->instance) return false;
    const float scale = std::fabs(right->t) > 1.f ? std::fabs(right->t) : 1.f;
    return std::fabs(left->t - right->t) <= 1e-3f * scale;
}

}  // namespace

TEST(d3d12_xs_acceleration_methods_follow_the_newer_list_shift) {
    const auto& earlier = list::functions(false);
    const auto& newer = list::functions(true);
    CHECK(earlier[kBuildSlot] != nullptr);
    CHECK(earlier[kEmitSlot] != nullptr);
    CHECK(earlier[kCopySlot] != nullptr);
    CHECK(earlier[kBatchedSlot] != nullptr);
    CHECK(newer[kBuildSlot + kNewerShift] == earlier[kBuildSlot]);
    CHECK(newer[kEmitSlot + kNewerShift] == earlier[kEmitSlot]);
    CHECK(newer[kCopySlot + kNewerShift] == earlier[kCopySlot]);
    CHECK(newer[kBatchedSlot + kNewerShift] == earlier[kBatchedSlot]);
    CHECK(newer[52] == earlier[52]);
}

TEST(d3d12_xs_command_list_builds_copies_and_queries_an_acceleration_structure) {
    if (host::host() == nullptr) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    const list::AccelerationMemory window = list::acceleration_memory();
    if (window.upload == nullptr || window.readback == nullptr || window.size < (1u << 20)) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    Object* allocator = nullptr;
    CHECK(list::create_allocator(nullptr, 0, &list::kAllocatorIids[0], &allocator) == S_OK);
    Object* commands = nullptr;
    CHECK(list::create_list(nullptr, 0, 0, allocator, nullptr, &list::kListIids[0], &commands) == S_OK);
    if (commands == nullptr) return;
    const auto& earlier = list::functions(false);
    const std::size_t shift = commands->functions[kBuildSlot] == earlier[kBuildSlot] ? 0 : kNewerShift;
    slot<void (*)(Object*, const void*, std::uint32_t, const void*)>(commands, (kBuildSlot + shift) * 8)(
        commands, nullptr, 0, nullptr);

    const float vertices[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    constexpr std::uint32_t kVertices = 256;
    constexpr std::uint32_t kScratch = 64 * 1024;
    constexpr std::uint32_t kInstances = 512;
    std::memcpy(window.upload + kVertices, vertices, sizeof vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC geometry = triangle_desc(kVertices);
    bvh::PrebuildSizes sizes{};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geometry;
    CHECK(bvh::prebuild_sizes(inputs, &sizes) == S_OK);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.NumDescs = 1;
    bvh::PrebuildSizes tlas_sizes{};
    CHECK(bvh::prebuild_sizes(tlas_inputs, &tlas_sizes) == S_OK);
    const std::uint64_t scratch_need = sizes.scratch > tlas_sizes.scratch ? sizes.scratch : tlas_sizes.scratch;
    CHECK(scratch_need <= 256 * 1024);
    bvh::Layout layout{};
    CHECK(bvh::compute_layout(1, false, false, &layout));
    const std::uint32_t blas = 1024 * 1024;
    const std::uint32_t clone = blas + static_cast<std::uint32_t>(layout.size);
    const std::uint32_t packed = clone + static_cast<std::uint32_t>(layout.size);
    const std::uint32_t info = packed + static_cast<std::uint32_t>(layout.size);
    CHECK(info + 16 < window.size);
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    std::memcpy(instance.Transform, identity, sizeof identity);
    instance.InstanceID = 7;
    instance.InstanceMask = 0xff;
    instance.AccelerationStructure = blas;
    std::memcpy(window.upload + kInstances, &instance, sizeof instance);

    BatchedBuild build{};
    build.desc.DestAccelerationStructureData = blas;
    build.desc.ScratchAccelerationStructureData = kScratch;
    build.desc.Inputs = inputs;
    slot<void (*)(Object*, const void*, std::uint32_t, const void*)>(commands, (kBuildSlot + shift) * 8)(
        commands, &build, 0, nullptr);

    slot<void (*)(Object*, std::uint64_t, std::uint64_t, std::uint32_t)>(commands, (kCopySlot + shift) * 8)(
        commands, clone, blas, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{};
    post.DestBuffer = info;
    post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    const std::uint64_t source = blas;
    slot<void (*)(Object*, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*, std::uint32_t,
                  const std::uint64_t*)>(commands, (kEmitSlot + shift) * 8)(commands, &post, 1, &source);
    slot<void (*)(Object*, std::uint64_t, std::uint64_t, std::uint32_t)>(commands, (kCopySlot + shift) * 8)(
        commands, packed, blas, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);


    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC current{};
    current.DestBuffer = info + 8;
    current.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
    bvh::Layout tlas_layout{};
    CHECK(bvh::compute_layout(1, true, false, &tlas_layout));
    const std::uint32_t tlas = (info + 256u + 255u) & ~255u;
    CHECK(tlas + tlas_layout.size < window.size);
    BatchedBuild batched{};
    batched.desc.DestAccelerationStructureData = tlas;
    batched.desc.ScratchAccelerationStructureData = kScratch;
    batched.desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    batched.desc.Inputs.NumDescs = 1;
    batched.desc.Inputs.InstanceDescs = kInstances;
    batched.postbuild_count = 1;
    batched.postbuild = &current;
    slot<void (*)(Object*, std::uint32_t, const void*)>(commands, (kBatchedSlot + shift) * 8)(commands, 1, &batched);

    CHECK(slot<HRESULT (*)(Object*)>(commands, 0x48)(commands) == S_OK);
    queue::Desc queue_desc{};
    Object* queue_object = nullptr;
    CHECK(queue::create(nullptr, &queue_desc, &queue::kIids[0], &queue_object) == S_OK);
    slot<void (*)(Object*, std::uint32_t, Object* const*)>(queue_object, 0x50)(queue_object, 1, &commands);
    CHECK(host::host()->device()->GetDeviceRemovedReason() == S_OK);

    const std::uint8_t* built = window.readback + blas;
    const std::uint32_t bytes = header_bytes(built);
    CHECK(bytes >= bvh::kHeaderSize);
    CHECK(bytes <= layout.size);
    CHECK(bvh::reference::validate({built, bytes}, true).empty());
    std::uint64_t compacted = 0;
    std::uint64_t current_size = 0;
    std::memcpy(&compacted, window.readback + info, sizeof compacted);
    std::memcpy(&current_size, window.readback + info + 8, sizeof current_size);
    CHECK(compacted == bytes);
    CHECK(current_size == header_bytes(window.readback + tlas));
    CHECK(std::memcmp(window.readback + clone, built, bytes) == 0);
    CHECK(bvh::reference::validate({window.readback + packed, header_bytes(window.readback + packed)}, true).empty());
    CHECK(bvh::reference::validate({window.readback + tlas, header_bytes(window.readback + tlas)}, false).empty());

    bvh::reference::Triangle triangle{};
    for (int corner = 0; corner < 3; ++corner)
        for (int axis = 0; axis < 3; ++axis) triangle.vertices[corner][axis] = vertices[corner * 3 + axis];
    bvh::reference::SceneInstance scene;
    scene.mask = 0xff;
    scene.triangles.push_back(triangle);
    bvh::reference::Memory memory;
    memory.place(blas, {built, bytes});
    memory.place(tlas, {window.readback + tlas, header_bytes(window.readback + tlas)});
    for (std::uint32_t ray_index = 0; ray_index < kRayCount; ++ray_index) {
        bvh::reference::Ray ray{};
        ray.origin[0] = 0.2f + 0.05f * static_cast<float>(ray_index);
        ray.origin[1] = 0.2f;
        ray.origin[2] = 2.f;
        ray.direction[2] = -1.f;
        ray.t_max = 10.f;
        ray.mask = 0xff;
        const auto brute = bvh::reference::brute_force(std::span<const bvh::reference::SceneInstance>(&scene, 1), ray);
        CHECK(same_hit(bvh::reference::trace(memory, blas, ray), brute));
        const auto top = bvh::reference::trace(memory, tlas, ray);
        CHECK(top.has_value() == brute.has_value());
        if (top && brute) {
            CHECK(top->instance == 0u);
            CHECK(std::fabs(top->t - brute->t) <= 1e-3f);
        }
    }
    slot<std::uint32_t (*)(Object*)>(commands, 0x10)(commands);
    slot<std::uint32_t (*)(Object*)>(allocator, 0x10)(allocator);
    slot<std::uint32_t (*)(Object*)>(queue_object, 0x10)(queue_object);
}
