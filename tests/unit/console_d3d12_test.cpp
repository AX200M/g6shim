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
