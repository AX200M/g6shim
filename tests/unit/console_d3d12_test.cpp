// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "check.hpp"

#include "console_objects.hpp"
#include "frame_clock.hpp"

using namespace g6shim::d3d12;

TEST(console_queue_types_map_to_pc_types) {
    D3D12_COMMAND_LIST_TYPE type{};
    CHECK(pc_queue_type(kConsoleQueueTypeDirect, &type) && type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    CHECK(pc_queue_type(kConsoleQueueTypeCompute, &type) && type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    CHECK(pc_queue_type(kConsoleQueueTypeDma, &type) && type == D3D12_COMMAND_LIST_TYPE_COPY);
    CHECK(!pc_queue_type(1, &type));
    CHECK(!pc_queue_type(17, &type));
}

TEST(console_sampler_heaps_fit_the_pc_limit) {
    D3D12_DESCRIPTOR_HEAP_DESC console{static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(kConsoleHeapTypeSamplerUnbounded), 16384,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC pc = pc_heap_desc(console);
    CHECK(pc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    CHECK(pc.NumDescriptors == D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
    console.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(pc_heap_desc(console).NumDescriptors == 16384);
    console = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 100000, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    pc = pc_heap_desc(console);
    CHECK(pc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CHECK(pc.NumDescriptors == 100000);
    CHECK(pc_heap_type(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

TEST(console_options_structure_is_larger) {
    CHECK(pc_feature_data_bytes(D3D12_FEATURE_D3D12_OPTIONS, kConsoleOptionsBytes) ==
          sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS));
    CHECK(pc_feature_data_bytes(D3D12_FEATURE_D3D12_OPTIONS, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) ==
          sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS));
    CHECK(pc_feature_data_bytes(D3D12_FEATURE_FEATURE_LEVELS, 0x18) == 0x18);
}

TEST(frame_clock_spaces_events_by_type) {
    FrameClock clock;
    clock.interval_us = 16667;
    clock.origin_period = 2;
    CHECK(clock.every_us(kFrameEventOrigin) == 33334);
    CHECK(clock.every_us(kFrameEventFlip) == 16667);
    CHECK(clock.event_time_us(kFrameEventFlip, 3) == 50001);
    CHECK(clock.first_index_at_or_after(kFrameEventFlip, 0) == 0);
    CHECK(clock.first_index_at_or_after(kFrameEventFlip, 1) == 1);
    CHECK(clock.first_index_at_or_after(kFrameEventFlip, 16667) == 1);
    CHECK(clock.first_index_at_or_after(kFrameEventFlip, 16668) == 2);
    clock.offset_us[kFrameEventVblank] = 1000;
    CHECK(clock.first_index_at_or_after(kFrameEventVblank, 500) == 0);
    CHECK(clock.event_time_us(kFrameEventVblank, 1) == 17667);
    CHECK(frame_interval_valid(16667, 1));
    CHECK(!frame_interval_valid(0, 1));
    CHECK(!frame_interval_valid(16667, 0));
}

TEST(console_graphics_list_slots_map_to_pc_slots) {
    CHECK(pc_slot_of_list_slot(0) == 0);
    CHECK(pc_slot_of_list_slot(9) == pc_list_slot::kClose);
    CHECK(pc_slot_of_list_slot(list_slot::kResourceBarrier) == pc_list_slot::kResourceBarrier);
    CHECK(pc_slot_of_list_slot(list_slot::kFirstQuery) == pc_list_slot::kBeginQuery);
    CHECK(pc_slot_of_list_slot(list_slot::kSetDescriptorHeaps) == pc_list_slot::kSetDescriptorHeaps);
    CHECK(pc_slot_of_list_slot(51) == 47);
    CHECK(pc_slot_of_list_slot(52) == 48);
    CHECK(pc_slot_of_list_slot(55) == 51);
    CHECK(pc_slot_of_list_slot(56) == pc_list_slot::kSetMarker);
    CHECK(pc_slot_of_list_slot(pc_list_slot::kExecuteIndirect) == pc_list_slot::kExecuteIndirect);
    CHECK(pc_slot_of_list_slot(list_slot::kMappedEnd) == kNoPcSlot);
    bool seen[list_slot::kMappedEnd] = {};
    for (std::size_t slot = 0; slot < list_slot::kMappedEnd; ++slot) {
        const std::size_t pc = pc_slot_of_list_slot(slot);
        CHECK(pc < list_slot::kMappedEnd && !seen[pc]);
        if (pc < list_slot::kMappedEnd) seen[pc] = true;
    }
}

TEST(console_dma_list_slots_map_to_pc_copy_list_slots) {
    CHECK(pc_slot_of_dma_list_slot(2) == 2);
    CHECK(pc_slot_of_dma_list_slot(dma_list_slot::kGetType) == kNoPcSlot);
    CHECK(pc_slot_of_dma_list_slot(9) == pc_list_slot::kClose);
    CHECK(pc_slot_of_dma_list_slot(10) == pc_list_slot::kReset);
    CHECK(pc_slot_of_dma_list_slot(11) == pc_list_slot::kCopyBufferRegion);
    CHECK(pc_slot_of_dma_list_slot(dma_list_slot::kResourceBarrier) == pc_list_slot::kResourceBarrier);
    CHECK(pc_slot_of_dma_list_slot(21) == pc_list_slot::kEndEvent);
    CHECK(pc_slot_of_dma_list_slot(22) == kNoPcSlot);
}

TEST(dma_list_barriers_keep_only_copy_states) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    CHECK(kept_on_copy_list(barrier));
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    CHECK(kept_on_copy_list(barrier));
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER;
    CHECK(!kept_on_copy_list(barrier));
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    CHECK(!kept_on_copy_list(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    CHECK(kept_on_copy_list(barrier));
}

TEST(buffers_drop_the_shader_resource_denial) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    CHECK(pc_resource_desc(desc).Flags == D3D12_RESOURCE_FLAG_NONE);
    desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHECK(pc_resource_desc(desc).Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK(pc_resource_desc(desc).Flags == desc.Flags);
}
