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
    CHECK(shader_visible(console));
    console.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(!shader_visible(console));
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

TEST(textures_created_for_copies_start_common) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    CHECK(pc_initial_state(&desc, D3D12_RESOURCE_STATE_COPY_DEST) == D3D12_RESOURCE_STATE_COMMON);
    CHECK(pc_initial_state(&desc, D3D12_RESOURCE_STATE_COPY_SOURCE) == D3D12_RESOURCE_STATE_COMMON);
    CHECK(pc_initial_state(&desc, D3D12_RESOURCE_STATE_RENDER_TARGET) == D3D12_RESOURCE_STATE_RENDER_TARGET);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    CHECK(pc_initial_state(&desc, D3D12_RESOURCE_STATE_COPY_DEST) == D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(pc_initial_state(nullptr, D3D12_RESOURCE_STATE_COPY_DEST) == D3D12_RESOURCE_STATE_COPY_DEST);
}

TEST(frame_tokens_carry_the_event_type) {
    CHECK(frame_token(kFrameEventOrigin, 0) == 1);
    CHECK(frame_token(kFrameEventOrigin, 41) == 42);
    CHECK(frame_token(kFrameEventFlip, 0) == ((std::uint64_t{kFrameEventFlip} << kFrameTokenTypeShift) | 1));
}

TEST(frame_event_waits_hand_out_growing_tokens) {
    CHECK(device_set_frame_interval(nullptr, nullptr, 1000, 1, 0) == S_OK);
    UINT64 first = 0;
    UINT64 second = 0;
    CHECK(device_wait_frame_event(nullptr, kFrameEventOrigin, INFINITE, nullptr, 0, &first) == S_OK);
    CHECK(device_wait_frame_event(nullptr, kFrameEventOrigin, INFINITE, nullptr, 0, &second) == S_OK);
    CHECK(first != 0 && second > first);
    CHECK(device_wait_frame_event(nullptr, kFrameEventTypes, INFINITE, nullptr, 0, &second) == E_INVALIDARG);
    CHECK(device_set_frame_interval(nullptr, nullptr, 1000000, 1, 0) == S_OK);
    CHECK(device_wait_frame_event(nullptr, kFrameEventOrigin, 1, nullptr, 0, &second) ==
          HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(device_set_frame_interval(nullptr, nullptr, 16667, 1, 0) == S_OK);
}

TEST(texture_transitions_drop_buffer_states) {
    D3D12_RESOURCE_BARRIER console{};
    console.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    console.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    console.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(0x8C3);
    D3D12_RESOURCE_BARRIER pc{};
    CHECK(pc_transition(console, true, &pc));
    CHECK(pc.Transition.StateAfter == static_cast<D3D12_RESOURCE_STATES>(0x8C0));
    CHECK(pc_transition(console, false, &pc) && pc.Transition.StateAfter == console.Transition.StateAfter);
    console.Transition.StateBefore = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    console.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    CHECK(!pc_transition(console, true, &pc));
    console.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    CHECK(pc_transition(console, true, &pc));
}
