// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/object.hpp"
#include "d3d12_xs/root.hpp"

#include <d3d12.h>

#include <cstring>

using namespace g6shim::d3d12_xs;

TEST(d3d12_xs_host_serialized_blobs_create_root_signatures) {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.RegisterSpace = 0;
    parameter.Constants.Num32BitValues = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root::Desc desc{};
    desc.parameter_count = 1;
    desc.parameters = &parameter;
    desc.flags = 0x0100'0001;
    void* blob = nullptr;
    void* error = nullptr;
    CHECK(root::serialize(&desc, 1, &blob, &error) == S_OK);
    CHECK(blob != nullptr);
    if (blob == nullptr) return;
    auto* blob_object = static_cast<Object*>(blob);
    const auto data = slot<const std::uint8_t* (*)(Object*)>(blob_object, 0x18)(blob_object);
    const auto size = slot<std::size_t (*)(Object*)>(blob_object, 0x20)(blob_object);
    CHECK(size > 0);
    Object* created = nullptr;
    CHECK(root::create(nullptr, 0, data, size, &root::kIids[0], &created) == S_OK);
    CHECK(slot<std::uint32_t (*)(Object*)>(created, 0x10)(created) == 0);
    slot<std::uint32_t (*)(Object*)>(blob_object, 0x10)(blob_object);
}

TEST(d3d12_xs_our_blobs_deserialize_on_the_host) {
    root::VersionedDesc desc{};
    desc.version = 1;
    desc.desc.flags = 1;
    void* blob = nullptr;
    void* error = nullptr;
    CHECK(root::serialize_versioned(&desc, &blob, &error) == S_OK);
    CHECK(blob != nullptr);
    if (blob == nullptr) return;
    auto* blob_object = static_cast<Object*>(blob);
    const auto data = slot<const std::uint8_t* (*)(Object*)>(blob_object, 0x18)(blob_object);
    const auto size = slot<std::size_t (*)(Object*)>(blob_object, 0x20)(blob_object);
    const GUID id = __uuidof(ID3D12RootSignatureDeserializer);
    Iid iid{};
    std::memcpy(iid.data(), &id, sizeof id);
    void* out = nullptr;
    CHECK(root::create_deserializer(data, size, &iid, &out) == S_OK);
    CHECK(out != nullptr);
    if (out != nullptr) {
        auto* deserializer = static_cast<ID3D12RootSignatureDeserializer*>(out);
        const D3D12_ROOT_SIGNATURE_DESC* read = deserializer->GetRootSignatureDesc();
        CHECK(read != nullptr);
        if (read != nullptr) {
            CHECK(read->NumParameters == 0);
            CHECK(read->Flags == D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        }
        deserializer->Release();
    }
    slot<std::uint32_t (*)(Object*)>(blob_object, 0x10)(blob_object);
}

TEST(d3d12_xs_versioned_descriptions_serialize_on_the_host) {
    root::VersionedDesc desc{};
    desc.version = 1;
    desc.desc.flags = 0x0200'0001;
    void* blob = nullptr;
    void* error = nullptr;
    CHECK(root::serialize_versioned(&desc, &blob, &error) == S_OK);
    CHECK(blob != nullptr);
    if (blob == nullptr) return;
    auto* blob_object = static_cast<Object*>(blob);
    CHECK(slot<std::size_t (*)(Object*)>(blob_object, 0x20)(blob_object) > 0);
    slot<std::uint32_t (*)(Object*)>(blob_object, 0x10)(blob_object);
    blob = nullptr;
    CHECK(root::serialize_versioned(nullptr, &blob, &error) == E_INVALIDARG);
}
