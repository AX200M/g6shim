// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/device.hpp"
#include "d3d12_xs/dxgi.hpp"
#include "d3d12_xs/object.hpp"

using namespace g6shim::d3d12_xs;

namespace {

using Query = HRESULT (*)(Object*, const Iid*, Object**);
using Get = HRESULT (*)(Object*, Object**);
using Indexed = HRESULT (*)(Object*, std::uint32_t, Object**);
using Describe = HRESULT (*)(Object*, dxgi::AdapterDescription*);

}  // namespace

TEST(d3d12_xs_startup_walks_from_device_to_output) {
    Object* d3d = device::device();
    const Iid newest_pc = guid(0x6007896c, 0x3244, 0x4afd, {0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23});
    Object* dxgi_device = nullptr;
    const auto qi = slot<Query>(d3d, 0);
    CHECK(qi(d3d, &newest_pc, &dxgi_device) == E_NOINTERFACE);
    CHECK(dxgi_device == nullptr);
    CHECK(qi(d3d, &dxgi::kDeviceIids[2], &dxgi_device) == S_OK);
    CHECK(dxgi_device == dxgi::device());

    Object* adapter = nullptr;
    CHECK(slot<Get>(dxgi_device, 0x38)(dxgi_device, &adapter) == S_OK);
    dxgi::AdapterDescription description{};
    CHECK(slot<Describe>(adapter, 0x40)(adapter, &description) == S_OK);
    CHECK(description.vendor == dxgi::kAmd);

    Object* factory = nullptr;
    const auto parent = slot<Query>(adapter, 0x30);
    CHECK(parent(adapter, &dxgi::kFactoryIids[2], &factory) == S_OK);
    Object* again = nullptr;
    CHECK(slot<Indexed>(factory, 0x38)(factory, 0, &again) == S_OK);
    CHECK(again == adapter);

    Object* output = nullptr;
    const auto outputs = slot<Indexed>(adapter, 0x38);
    CHECK(outputs(adapter, 0, &output) == S_OK);
    CHECK(output != nullptr);
    CHECK(outputs(adapter, 1, &output) == kNotFound);
}
