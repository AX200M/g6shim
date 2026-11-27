// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g6shim::d3d12_xs::gpu_capture {

struct Api {
    void (*get_version)(int* major, int* minor, int* patch);
    int (*set_option_u32)(int option, std::uint32_t value);
    int (*set_option_f32)(int option, float value);
    std::uint32_t (*get_option_u32)(int option);
    float (*get_option_f32)(int option);
    void (*set_focus_keys)(int* keys, int count);
    void (*set_capture_keys)(int* keys, int count);
    std::uint32_t (*get_overlay_bits)();
    void (*mask_overlay_bits)(std::uint32_t keep, std::uint32_t set);
    void (*remove_hooks)();
    void (*unload_crash_handler)();
    void (*set_path_template)(const char* path);
    const char* (*get_path_template)();
    std::uint32_t (*capture_count)();
    std::uint32_t (*get_capture)(std::uint32_t index, char* path, std::uint32_t* length, std::uint64_t* time);
    void (*trigger)();
};

inline constexpr int kApiVersion = 10600;
inline constexpr int kOptionSoftMemoryLimit = 13;
inline constexpr std::uint32_t kSoftMemoryLimitMegabytes = 1000;

void load();
void frame_presented();

}  // namespace g6shim::d3d12_xs::gpu_capture
