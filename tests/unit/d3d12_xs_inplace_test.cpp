// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/inplace.hpp"
#include "d3d12_xs/shadow.hpp"

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using namespace g6shim::d3d12_xs;
using Microsoft::WRL::ComPtr;

constexpr std::size_t kUnit = 64 << 10;

host::Host* inplace_host() {
    ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
    return host::host();
}

bool opens_existing_heaps(ID3D12Device* device) {
    ComPtr<ID3D12Device13> device13;
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device13)));
}

ComPtr<ID3D12Resource> readback_buffer(ID3D12Device* device, std::uint64_t bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&made))))
        return nullptr;
    return made;
}

std::vector<std::uint8_t> card_reads(host::Host& gpu, const inplace::Region& region, std::uint64_t address,
                                     std::size_t bytes) {
    const auto readback = readback_buffer(gpu.device(), bytes);
    const auto list = gpu.new_list();
    if (!readback || !list) return {};
    list->list()->CopyBufferRegion(readback.Get(), 0, region.buffer(), address - region.base(), bytes);
    list->close();
    gpu.execute({list.get()});
    host::wait_idle();
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) return {};
    std::vector<std::uint8_t> out(static_cast<const std::uint8_t*>(mapped),
                                  static_cast<const std::uint8_t*>(mapped) + bytes);
    readback->Unmap(0, nullptr);
    return out;
}

bool card_writes(host::Host& gpu, const inplace::Region& region, std::uint64_t address, std::uint8_t value,
                 std::size_t bytes) {
    const auto upload = gpu.upload_buffer(bytes);
    const auto list = gpu.new_list();
    if (!upload || !list) return false;
    std::memset(upload->second, value, bytes);
    list->list()->CopyBufferRegion(region.buffer(), address - region.base(), upload->first.Get(), 0, bytes);
    list->close();
    gpu.execute({list.get()});
    host::wait_idle();
    return true;
}

HANDLE g_section = nullptr;
std::uintptr_t g_view_base = 0;

BOOL WINAPI test_stretch(std::uintptr_t address, std::size_t bytes, HANDLE* section, std::uint64_t* offset) {
    if (address < g_view_base || address + bytes > g_view_base + 8 * kUnit) return FALSE;
    *section = g_section;
    *offset = address - g_view_base;
    return TRUE;
}

}  // namespace

TEST(d3d12_xs_inplace_private_memory_is_the_cards_own) {
    host::Host* gpu = inplace_host();
    if (gpu == nullptr || gpu->device() == nullptr || !opens_existing_heaps(gpu->device())) return;
    constexpr std::size_t kBytes = 4 * kUnit;
    auto* memory = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    for (std::size_t i = 0; i < kBytes; ++i) memory[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const auto start = reinterpret_cast<std::uint64_t>(memory) + 0x40;
    {
        const auto region = inplace::place(gpu->device(), start, kBytes - 0x80, true);
        CHECK(region != nullptr);
        if (region) {
            CHECK(region->backing() == inplace::Backing::kPrivate);
            CHECK(region->base() == reinterpret_cast<std::uint64_t>(memory));
            const auto seen = card_reads(*gpu, *region, start, 4096);
            CHECK(seen.size() == 4096);
            CHECK(seen.size() == 4096 && std::memcmp(seen.data(), memory + 0x40, 4096) == 0);
            CHECK(card_writes(*gpu, *region, start + kUnit, 0x5A, 256));
            bool written = true;
            for (std::size_t i = 0; i < 256; ++i) written = written && memory[0x40 + kUnit + i] == 0x5A;
            CHECK(written);
        }
    }
    CHECK(::VirtualFree(memory, 0, MEM_RELEASE) != 0);
}

TEST(d3d12_xs_inplace_view_memory_is_opened_through_a_view_of_ours) {
    host::Host* gpu = inplace_host();
    if (gpu == nullptr || gpu->device() == nullptr || !opens_existing_heaps(gpu->device())) return;
    g_section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0,
                                     static_cast<DWORD>(8 * kUnit), nullptr);
    CHECK(g_section != nullptr);
    if (g_section == nullptr) return;
    auto* hole = static_cast<std::uint8_t*>(::VirtualAlloc2(::GetCurrentProcess(), nullptr, 8 * kUnit,
                                                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                                             nullptr, 0));
    CHECK(hole != nullptr);
    if (hole == nullptr) return;
    ::VirtualFree(hole, 3 * kUnit, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    std::vector<void*> views;
    for (const auto& [page, count] : {std::pair<std::size_t, std::size_t>{0, 3}, {3, 5}}) {
        void* view = ::MapViewOfFile3(g_section, ::GetCurrentProcess(), hole + page * kUnit, page * kUnit,
                                      count * kUnit, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        CHECK(view != nullptr);
        if (view == nullptr) return;
        CHECK(::VirtualAlloc(view, count * kUnit, MEM_COMMIT, PAGE_READWRITE) != nullptr);
        views.push_back(view);
    }
    g_view_base = reinterpret_cast<std::uintptr_t>(hole);
    inplace::use_stretch_finder(&test_stretch);
    for (std::size_t i = 0; i < 8 * kUnit; ++i) hole[i] = static_cast<std::uint8_t>(i * 5 + 1);
    const auto start = reinterpret_cast<std::uint64_t>(hole) + kUnit;
    {
        const auto region = inplace::place(gpu->device(), start, 5 * kUnit, true);
        CHECK(region != nullptr);
        if (region) {
            CHECK(region->backing() == inplace::Backing::kView);
            const auto seen = card_reads(*gpu, *region, start + 2 * kUnit - 128, 256);
            CHECK(seen.size() == 256 && std::memcmp(seen.data(), hole + 3 * kUnit - 128, 256) == 0);
            CHECK(card_writes(*gpu, *region, start + 3 * kUnit, 0xA5, 128));
            bool written = true;
            for (std::size_t i = 0; i < 128; ++i) written = written && hole[4 * kUnit + i] == 0xA5;
            CHECK(written);
        }
    }
    inplace::use_stretch_finder(nullptr);
    for (void* view : views) CHECK(::UnmapViewOfFile2(::GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER) != 0);
    ::VirtualFree(hole, 0, MEM_RELEASE);
    ::VirtualFree(hole + 3 * kUnit, 0, MEM_RELEASE);
    ::CloseHandle(g_section);
    g_section = nullptr;
}

TEST(d3d12_xs_inplace_buffer_offsets_count_from_the_placed_buffer) {
    host::Host* gpu = inplace_host();
    if (gpu == nullptr || gpu->device() == nullptr || !opens_existing_heaps(gpu->device())) return;
    constexpr std::size_t kBytes = 2 * kUnit;
    constexpr std::uint64_t kBias = 0x1230;
    auto* memory = static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    for (std::size_t i = 0; i < kBytes; ++i) memory[i] = static_cast<std::uint8_t>(i * 11 + 5);
    const auto start = reinterpret_cast<std::uint64_t>(memory) + kBias;
    {
        auto region = inplace::place(gpu->device(), start, 4096, false);
        CHECK(region != nullptr);
        if (region) {
            shadow::Shadow copy(region->buffer_ref(), reinterpret_cast<std::uint8_t*>(start), region->gpu(start),
                                start, 4096);
            copy.place_in(region);
            CHECK(copy.offset(start) == kBias);
            CHECK(copy.offset(start + 16) == kBias + 16);
            CHECK(copy.gpu(start) == region->buffer()->GetGPUVirtualAddress() + kBias);
            const auto readback = readback_buffer(gpu->device(), 64);
            const auto list = gpu->new_list();
            CHECK((readback && list));
            if (readback && list) {
                list->list()->CopyBufferRegion(readback.Get(), 0, copy.buffer(), copy.offset(start + 16), 64);
                list->close();
                gpu->execute({list.get()});
                host::wait_idle();
                void* mapped = nullptr;
                CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
                if (mapped != nullptr) {
                    CHECK(std::memcmp(mapped, memory + kBias + 16, 64) == 0);
                    readback->Unmap(0, nullptr);
                }
            }
        }
    }
    ::VirtualFree(memory, 0, MEM_RELEASE);
}

TEST(d3d12_xs_inplace_turns_down_memory_it_cannot_open) {
    host::Host* gpu = inplace_host();
    if (gpu == nullptr || gpu->device() == nullptr) return;
    auto* reserved = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 2 * kUnit, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(reserved != nullptr);
    if (reserved == nullptr) return;
    const std::uint64_t before = inplace::stats().not_one_kind;
    CHECK(inplace::place(gpu->device(), reinterpret_cast<std::uint64_t>(reserved), kUnit, true) == nullptr);
    CHECK(inplace::stats().not_one_kind == before + 1);
    ::VirtualFree(reserved, 0, MEM_RELEASE);
}
