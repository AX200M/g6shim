// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/indirect.hpp"

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <stdlib.h>

using namespace g6shim::d3d12_xs;

namespace {

struct WarpEnvironment {
    WarpEnvironment() { ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1"); }
};

const WarpEnvironment g_warp_environment;

}  // namespace

TEST(d3d12_xs_argument_clamp_builds) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    if (g6shim::d3d12_xs::host::host() == nullptr) return;
    CHECK(g6shim::d3d12_xs::indirect::clamp_available());
}

TEST(d3d12_xs_indexed_dispatch_after_sanitize_reads_the_descriptor) {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    ::SetEnvironmentVariableA("G6SHIM_D3D_DEBUG", "1");
    using Microsoft::WRL::ComPtr;
    host::Host* gpu = host::host();
    if (gpu == nullptr || gpu->device() == nullptr) return;
    const char* dxc = "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe";
    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) == 0) return;
    const std::wstring directory(temp);
    const std::wstring source_path = directory + L"g6shim-indexed.hlsl";
    const std::wstring dxil_path = directory + L"g6shim-indexed.dxil";
    {
        std::ofstream source(source_path, std::ios::binary);
        source << "RWByteAddressBuffer output : register(u0);\n"
                  "[RootSignature(\"RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED), UAV(u0)\")]\n"
                  "[numthreads(1, 1, 1)]\n"
                  "void main() {\n"
                  "    ByteAddressBuffer source = ResourceDescriptorHeap[0];\n"
                  "    output.Store(0, source.Load(0));\n"
                  "}\n";
    }
    std::wstring command = L"\"";
    command += std::wstring(dxc, dxc + std::strlen(dxc));
    command += L"\" -T cs_6_6 -E main -Qstrip_rootsignature -Fo \"" + dxil_path + L"\" \"" + source_path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process)) {
        std::printf("  skipped: shader compiler\n");
        return;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (status != 0) {
        std::printf("  skipped: shader compiler\n");
        return;
    }
    std::ifstream dxil_file(dxil_path, std::ios::binary);
    std::vector<char> dxil((std::istreambuf_iterator<char>(dxil_file)), {});
    if (dxil.size() < 4) return;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> serialized;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)));
    if (!serialized) return;
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(gpu->device()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                       IID_PPV_ARGS(&root))));
    if (!root) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {dxil.data(), dxil.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
    if (!pipeline) return;

    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc = {1, 0};
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> arguments;
    CHECK(SUCCEEDED(gpu->device()->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &buffer,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&source))));
    CHECK(SUCCEEDED(gpu->device()->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &buffer,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&output))));
    buffer.Width = 12;
    CHECK(SUCCEEDED(gpu->device()->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &buffer,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&arguments))));
    if (!source || !output || !arguments) return;
    constexpr std::uint32_t kMagic = 0xA11CE001u;
    const auto upload = gpu->upload_buffer(sizeof kMagic);
    CHECK(upload.has_value());
    if (!upload) return;
    std::memcpy(upload->second, &kMagic, sizeof kMagic);
    const auto seed = gpu->new_list();
    if (!seed) return;
    seed->list()->CopyBufferRegion(source.Get(), 0, upload->first.Get(), 0, sizeof kMagic);
    D3D12_RESOURCE_BARRIER to_read{};
    to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_read.Transition.pResource = source.Get();
    to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    seed->list()->ResourceBarrier(1, &to_read);
    seed->close();
    gpu->execute({seed.get()});
    host::wait_idle();

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    CHECK(SUCCEEDED(gpu->device()->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors))));
    if (!descriptors) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = 1;
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gpu->device()->CreateShaderResourceView(source.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());

    const auto list = gpu->new_list();
    if (!list) return;
    CHECK(indirect::bound(*list, arguments.Get(), arguments->GetGPUVirtualAddress(), 2, 12, 1));
    ID3D12DescriptorHeap* heaps[] = {descriptors.Get()};
    list->list()->SetDescriptorHeaps(1, heaps);
    list->list()->SetComputeRootSignature(root.Get());
    list->list()->SetPipelineState(pipeline.Get());
    list->list()->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->list()->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = output.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->list()->ResourceBarrier(1, &to_copy);
    const auto readback = gpu->shared_buffer(sizeof kMagic);
    CHECK(readback.has_value());
    if (!readback) return;
    list->list()->CopyBufferRegion(readback->first.Get(), 0, output.Get(), 0, sizeof kMagic);
    list->close();
    gpu->execute({list.get()});
    host::wait_idle();
    std::uint32_t loaded = 0;
    std::memcpy(&loaded, readback->second, sizeof loaded);
    CHECK(loaded == kMagic);
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(gpu->device()->QueryInterface(IID_PPV_ARGS(&messages)))) {
        const UINT64 count = messages->GetNumStoredMessages();
        bool ordering = false;
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T stored = 0;
            if (FAILED(messages->GetMessage(index, nullptr, &stored)) || stored == 0) continue;
            std::vector<std::uint8_t> storage(stored);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(index, message, &stored)) || message->pDescription == nullptr) continue;
            const std::string text(message->pDescription);
            if (text.find("SetDescriptorHeaps") != std::string::npos &&
                text.find("RootSignature") != std::string::npos)
                ordering = true;
        }
        CHECK(!ordering);
    }
}
