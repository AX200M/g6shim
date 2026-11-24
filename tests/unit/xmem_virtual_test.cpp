// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "virtual_memory.hpp"

#include <windows.h>

#include <cstdint>

using namespace g6shim::xmem;

extern "C" void* WINAPI g6shim_XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type, ULONGLONG flags,
                                                DWORD protect);
extern "C" BOOL WINAPI g6shim_XMemVirtualQuery(HANDLE process, const void* address, BasicInformation* out);

namespace {

BasicInformation query(const void* address) {
    BasicInformation info{};
    g6shim_XMemVirtualQuery(GetCurrentProcess(), address, &info);
    return info;
}

}  // namespace

TEST(xmem_address_ranges_follow_the_flags) {
    const AddressRange none = address_range(kGraphicsFlags);
    CHECK(none.lowest == kLowestTitleAddress);
    CHECK(none.highest == (std::size_t{1} << 40) - 1);
    CHECK(address_range(kGraphicsFlags | std::uint64_t{7} << kAddressFieldShift).highest ==
          (std::size_t{1} << 40) - 1);
    CHECK(address_range(kGraphicsFlags | std::uint64_t{1} << kAddressFieldShift).highest ==
          (std::size_t{1} << 46) - 1);
    CHECK(address_range(kGraphicsFlags | std::uint64_t{3} << kAddressFieldShift).highest ==
          (std::size_t{1} << 44) - 1);
    const AddressRange low = address_range(kAddress32BitFlags);
    CHECK(low.lowest == 0);
    CHECK(low.highest == 0xffffffff);
}

TEST(xmem_virtual_alloc_meets_console_constraints) {
    const DWORD gpu_read_write = 0x2000;
    void* p = g6shim_XMemVirtualAlloc(nullptr, 4 * kLargePageBytes, kLargePages2M | MEM_RESERVE | MEM_COMMIT,
                                     kGraphicsFlags | std::uint64_t{7} << kAddressFieldShift,
                                     PAGE_READWRITE | gpu_read_write);
    CHECK(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % kLargePageBytes == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(p) < (std::uintptr_t{1} << 40));
    const BasicInformation info = query(p);
    CHECK(info.host.State == MEM_COMMIT);
    CHECK(info.host.Protect == PAGE_READWRITE);
    CHECK(info.flags == kGraphicsFlags);
    void* found[4] = {};
    ULONG_PTR count = 4;
    ULONG granularity = 0;
    CHECK(GetWriteWatch(0, p, 4 * kLargePageBytes, found, &count, &granularity) == 0);
    CHECK(VirtualFree(p, 0, MEM_RELEASE) != 0);
    CHECK(query(p).flags == 0);
    void* big = g6shim_XMemVirtualAlloc(nullptr, std::size_t{16} << 30, MEM_RESERVE, kGraphicsFlags,
                                        PAGE_NOACCESS);
    CHECK(big != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(big) >= (std::uintptr_t{1} << 32));
    CHECK(reinterpret_cast<std::uintptr_t>(big) + (std::size_t{16} << 30) <= (std::uintptr_t{1} << 40));
    CHECK(VirtualFree(big, 0, MEM_RELEASE) != 0);
}

TEST(xmem_placeholder_reservations_take_replacement) {
    void* reserve = g6shim_XMemVirtualAlloc(nullptr, 64 * 1024, MEM_RESERVE | kPages64K, 0, PAGE_READWRITE);
    CHECK(reserve != nullptr);
    CHECK(query(reserve).host.State == MEM_RESERVE);
    CHECK(query(reserve).host.Protect == 0);
    void* split = g6shim_XMemVirtualAlloc(reserve, 4096, kPages64K | MEM_REPLACE_PLACEHOLDER, 0,
                                         PAGE_READWRITE);
    CHECK(split == reserve);
    CHECK(VirtualFree(reserve, 0, MEM_RELEASE) != 0);
}

TEST(xmem_virtual_query_reports_the_console_tail) {
    const BasicInformation nowhere = query(nullptr);
    CHECK(g6shim_XMemVirtualQuery(GetCurrentProcess(), nullptr, nullptr) == 0);
    CHECK(nowhere.host.State == MEM_FREE || nowhere.host.State == MEM_RESERVE);
    CHECK(nowhere.flags == 0);
    CHECK(nowhere.tag_code == 0);
}
