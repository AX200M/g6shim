// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../standins.hpp"

#include <g6shim/winrt/events.hpp>
#include <g6shim/winrt/object.hpp>
#include <g6shim/winrt/registry.hpp>

#include <windows.h>

#include <cstdint>

namespace g6shim::winrt {

namespace {

constexpr char kSensor[] = "Windows.Kinect.KinectSensor";
constexpr char kSensorObject6[] = "Windows.Kinect.KinectSensor#6";
constexpr char kFrameDescription[] = "Windows.Kinect.KinectSensor#6.4";
constexpr GUID kStatics = {0xC421D46B, 0x5084, 0x45D6, {0xBD, 0xF0, 0x57, 0x15, 0x1F, 0x81, 0xD0, 0xD9}};
constexpr std::uint32_t kFrameWidth = 512;
constexpr std::uint32_t kFrameHeight = 424;

constexpr std::size_t kGetDefault = 0;

namespace sensor {
constexpr std::size_t kRemoveEvent = 1;
constexpr std::size_t kOpen = 2;
constexpr std::size_t kClose = 3;
constexpr std::size_t kIsOpen = 4;
constexpr std::size_t kIsAvailable = 5;
constexpr std::size_t kObject6 = 6;
}  // namespace sensor

constexpr std::size_t kObject6FrameDescription = 4;
constexpr std::size_t kFrameWidthMethod = 0;
constexpr std::size_t kFrameHeightMethod = 1;

HRESULT __stdcall GetDefault(void*, void** out) {
    if (!out) return E_POINTER;
    static void* const one = make_probe(kSensor, kNoIid);
    *out = add_ref(one);
    return S_OK;
}

HRESULT __stdcall Sensor_Open(void*) { return S_OK; }

HRESULT __stdcall Sensor_Close(void*) { return S_OK; }

HRESULT __stdcall Sensor_RemoveEvent(void*, std::int64_t token) {
    remove_event_handler(token);
    return S_OK;
}

HRESULT __stdcall Sensor_get_False(void*, BYTE* value) {
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

HRESULT __stdcall Sensor_get_Object6(void*, void** object) {
    if (!object) return E_POINTER;
    *object = make_probe(kSensorObject6, kNoIid);
    return S_OK;
}

HRESULT __stdcall Object6_get_FrameDescription(void*, std::uint32_t, void** object) {
    if (!object) return E_POINTER;
    *object = make_probe(kFrameDescription, kNoIid);
    return S_OK;
}

HRESULT __stdcall Frame_get_Width(void*, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = kFrameWidth;
    return S_OK;
}

HRESULT __stdcall Frame_get_Height(void*, std::uint32_t* value) {
    if (!value) return E_POINTER;
    *value = kFrameHeight;
    return S_OK;
}

}  // namespace

void register_sensor() {
    register_interface(kSensor, kStatics, make_vtable({{kGetDefault, fn(&GetDefault)}}));
    register_interface(kSensor, kNoIid,
                       make_vtable({{sensor::kRemoveEvent, fn(&Sensor_RemoveEvent)},
                                    {sensor::kOpen, fn(&Sensor_Open)},
                                    {sensor::kClose, fn(&Sensor_Close)},
                                    {sensor::kIsOpen, fn(&Sensor_get_False)},
                                    {sensor::kIsAvailable, fn(&Sensor_get_False)},
                                    {sensor::kObject6, fn(&Sensor_get_Object6)}}));
    register_interface(kSensorObject6, kNoIid,
                       make_vtable({{kObject6FrameDescription, fn(&Object6_get_FrameDescription)}}));
    register_interface(kFrameDescription, kNoIid,
                       make_vtable({{kFrameWidthMethod, fn(&Frame_get_Width)}, {kFrameHeightMethod, fn(&Frame_get_Height)}}));
}

}  // namespace g6shim::winrt
