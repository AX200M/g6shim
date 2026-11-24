// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "virtual_memory.hpp"

#include <g6shim/log.hpp>

namespace g6shim::xmem {

AddressRange address_range(std::uint64_t flags) {
    if (flags & kAddress32BitFlags) return {0, 0xffffffff};
    const unsigned field = static_cast<unsigned>((flags >> kAddressFieldShift) & kAddressFieldMask);
    const unsigned bits = field == 0 ? kDefaultAddressBits : kAddressBitsBase - field;
    return {kLowestTitleAddress, (std::size_t{1} << bits) - 1};
}

void* allocate_virtual(void* base, std::size_t bytes, std::uint32_t type, std::uint64_t flags,
                       std::uint32_t protect) {
    const AddressRange range = address_range(flags);
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(range.lowest);
    requirements.HighestEndingAddress = reinterpret_cast<void*>(range.highest);
    requirements.Alignment = (type & (kLargePages2M | kPages64KBit)) == kLargePages2M ? kLargePageBytes : 0;
    std::uint32_t host_protect = protect & kHostProtections;
    if (host_protect == 0) host_protect = PAGE_NOACCESS;
    const bool placeholder = (type & kPages64K) == kPages64K && (type & MEM_COMMIT) == 0;
    if (placeholder && (type & MEM_REPLACE_PLACEHOLDER) != 0) return base;
    if (placeholder) host_protect = PAGE_NOACCESS;
    std::uint32_t host_type = type & ~(kLargePages2M | kPages64KBit);
    if (placeholder) host_type |= MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    const bool constrained = base == nullptr;
    if (constrained) {
        host_type |= MEM_RESERVE;
        if ((host_type & MEM_RESERVE_PLACEHOLDER) == 0) host_type |= MEM_WRITE_WATCH;
    }
    void* at = VirtualAlloc2(GetCurrentProcess(), base, bytes, host_type, host_protect,
                             constrained ? &parameter : nullptr, constrained ? 1u : 0u);
    if (constrained)
        G6SHIM_TRACE("XMemVirtualAlloc(%zu bytes, type 0x%lX, flags 0x%llX) -> %p", bytes,
                     static_cast<unsigned long>(host_type), static_cast<unsigned long long>(flags), at);
    return at;
}

}  // namespace g6shim::xmem

extern "C" void* WINAPI g6shim_XMemVirtualAlloc(void* base, SIZE_T bytes, DWORD type, ULONGLONG flags,
                                                DWORD protect) {
    return g6shim::xmem::allocate_virtual(base, bytes, type, flags, protect);
}

extern "C" BOOL WINAPI g6shim_XMemVirtualQuery(HANDLE process, const void* address,
                                               g6shim::xmem::BasicInformation* out) {
    if (!out) return 0;
    if (VirtualQueryEx(process, address, &out->host, sizeof out->host) == 0) return 0;
    out->flags = out->host.State == MEM_COMMIT ? g6shim::xmem::kGraphicsFlags : 0;
    out->tag_code = 0;
    out->tag[0] = out->tag[1] = out->tag[2] = out->tag[3] = 0;
    return 1;
}
