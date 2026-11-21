// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "packets.hpp"

#include "packet.hpp"
#include "placement.hpp"
#include "proxy.hpp"

#include <windows.h>
#include <d3d11_1.h>

#include <g6shim/log.hpp>

#include <cstring>
#include <mutex>
#include <set>

namespace g6shim::d3d {

namespace {

constexpr unsigned kConstantSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr std::uint32_t kConstantWindow = 4096;
constexpr std::uint32_t kVertexUploadLimit = 1u << 20;
constexpr std::uint64_t kConstantOffsetAlignment = 256;
constexpr std::uint64_t kConstantUnit = 16;
constexpr std::uint32_t kHeaderKindMask = 0xFFFF00FF;

void note(std::uint32_t header, const char* what, bool problem = false) {
    static std::mutex mutex;
    static std::set<std::uint32_t> seen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(header & kHeaderKindMask).second) return;
    }
    if (problem) G6SHIM_WARN("packet %08X: %s", header, what);
    else G6SHIM_TRACE("packet %08X: %s", header, what);
}

void set_views(ID3D11DeviceContext* c, unsigned stage, UINT slot, ID3D11ShaderResourceView* view) {
    switch (stage) {
        case kVertex: c->VSSetShaderResources(slot, 1, &view); break;
        case kHull: c->HSSetShaderResources(slot, 1, &view); break;
        case kDomain: c->DSSetShaderResources(slot, 1, &view); break;
        case kGeometry: c->GSSetShaderResources(slot, 1, &view); break;
        case kPixel: c->PSSetShaderResources(slot, 1, &view); break;
        case kCompute: c->CSSetShaderResources(slot, 1, &view); break;
    }
}

void set_sampler(ID3D11DeviceContext* c, unsigned stage, UINT slot, ID3D11SamplerState* sampler) {
    switch (stage) {
        case kVertex: c->VSSetSamplers(slot, 1, &sampler); break;
        case kHull: c->HSSetSamplers(slot, 1, &sampler); break;
        case kDomain: c->DSSetSamplers(slot, 1, &sampler); break;
        case kGeometry: c->GSSetSamplers(slot, 1, &sampler); break;
        case kPixel: c->PSSetSamplers(slot, 1, &sampler); break;
        case kCompute: c->CSSetSamplers(slot, 1, &sampler); break;
    }
}

void set_constants(ID3D11DeviceContext* c, unsigned stage, UINT slot, ID3D11Buffer* buffer) {
    switch (stage) {
        case kVertex: c->VSSetConstantBuffers(slot, 1, &buffer); break;
        case kHull: c->HSSetConstantBuffers(slot, 1, &buffer); break;
        case kDomain: c->DSSetConstantBuffers(slot, 1, &buffer); break;
        case kGeometry: c->GSSetConstantBuffers(slot, 1, &buffer); break;
        case kPixel: c->PSSetConstantBuffers(slot, 1, &buffer); break;
        case kCompute: c->CSSetConstantBuffers(slot, 1, &buffer); break;
    }
}

void set_constants1(ID3D11DeviceContext1* c, unsigned stage, UINT slot, ID3D11Buffer* buffer, UINT first, UINT count) {
    switch (stage) {
        case kVertex: c->VSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
        case kHull: c->HSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
        case kDomain: c->DSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
        case kGeometry: c->GSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
        case kPixel: c->PSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
        case kCompute: c->CSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
    }
}

ID3D11Buffer* window_buffer(ID3D11DeviceContext* c, unsigned stage, UINT slot) {
    static ID3D11Buffer* buffers[kShaderStages][kConstantSlots] = {};
    ID3D11Buffer*& b = buffers[stage][slot];
    if (!b) {
        ID3D11Device* device = nullptr;
        c->GetDevice(&device);
        if (!device) return nullptr;
        D3D11_BUFFER_DESC desc{kConstantWindow, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(device->CreateBuffer(&desc, nullptr, &b))) b = nullptr;
        device->Release();
    }
    return b;
}

bool locate(ID3D11Buffer* buffer, std::uint64_t value, BufferTarget& t) {
    std::uint64_t address_field = 0;
    std::memcpy(&address_field, reinterpret_cast<const std::uint8_t*>(buffer) + kBufferAddressField, sizeof address_field);
    Placement p;
    return resolve_buffer(value, address_field, placement_of(buffer, p) ? &p : nullptr, t);
}

void bind_vertex_buffer(ID3D11DeviceContext* c, std::uint32_t header, UINT slot, ID3D11Buffer* buffer,
                        std::uint64_t value, const BufferTarget& t) {
    const UINT stride = buffer_stride(value);
    const UINT offset = static_cast<UINT>(t.offset);
    if (t.memory) {
        const std::uint32_t bytes = t.available < kVertexUploadLimit ? t.available : kVertexUploadLimit;
        const D3D11_BOX box{offset, 0, 0, offset + bytes, 1, 1};
        c->UpdateSubresource(buffer, 0, &box, t.memory, 0, 0);
    }
    c->IASetVertexBuffers(slot, 1, &buffer, &stride, &offset);
    note(header, "vertex buffer");
}

void bind_constants_at_offset(ID3D11DeviceContext* c, std::uint32_t header, unsigned stage, UINT slot,
                              ID3D11Buffer* buffer, const BufferTarget& t) {
    static ID3D11DeviceContext1* c1 = nullptr;
    if (!c1) c->QueryInterface(IID_PPV_ARGS(&c1));
    if (c1 && t.offset % kConstantOffsetAlignment == 0) {
        set_constants1(c1, stage, slot, buffer, static_cast<UINT>(t.offset / kConstantUnit), kConstantWindow / kConstantUnit);
        note(header, "constant buffer at an offset");
    } else {
        note(header, "constant buffer at an unaligned offset: not bound", true);
    }
}

void bind_constant_window(ID3D11DeviceContext* c, std::uint32_t header, unsigned stage, UINT slot, const BufferTarget& t) {
    ID3D11Buffer* window = window_buffer(c, stage, slot);
    if (!window) return;
    alignas(16) static std::uint8_t data[kConstantWindow];
    const std::uint32_t bytes = t.available < kConstantWindow ? t.available : kConstantWindow;
    std::memcpy(data, t.memory, bytes);
    std::memset(data + bytes, 0, kConstantWindow - bytes);
    c->UpdateSubresource(window, 0, nullptr, data, 0, 0);
    set_constants(c, stage, slot, window);
    note(header, "constant buffer in game memory (copied)");
}

void bind_buffer(ID3D11DeviceContext* c, std::uint32_t header, unsigned stage, UINT slot, ID3D11Buffer* buffer,
                 std::uint64_t value) {
    BufferTarget t;
    if (buffer && !locate(buffer, value, t)) {
        note(header, "buffer address outside its placement memory: not bound", true);
        return;
    }
    if (stage == kInputAssembler) {
        bind_vertex_buffer(c, header, slot, buffer, value, t);
    } else if (!buffer || (!t.memory && t.offset == 0)) {
        set_constants(c, stage, slot, buffer);
        note(header, "constant buffer");
    } else if (!t.memory) {
        bind_constants_at_offset(c, header, stage, slot, buffer, t);
    } else {
        bind_constant_window(c, header, stage, slot, t);
    }
}

UINT slot_limit(const Packet& p) {
    switch (p.type) {
        case kPacketView: return D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        case kPacketSampler: return D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        default: return p.stage == kInputAssembler ? D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT : kConstantSlots;
    }
}

}  // namespace

HRESULT STDMETHODCALLTYPE ContextPacket(void* self, const std::uint8_t* begin, const std::uint8_t* end) {
    const auto packet = parse_packet(begin, end);
    if (!self || !packet) return S_OK;
    const Packet& p = *packet;
    auto* c = static_cast<ID3D11DeviceContext*>(real_object(self));
    if (!known_stage(p)) {
        note(p.header, "unknown stage: ignored", true);
        return S_OK;
    }
    const UINT limit = slot_limit(p);
    for (std::size_t i = 0; i < p.count; ++i) {
        const PacketEntry e = p.entry(i);
        const UINT slot = p.first_slot + static_cast<UINT>(i);
        if (slot >= limit) break;
        switch (p.type) {
            case kPacketView:
                set_views(c, p.stage, slot, static_cast<ID3D11ShaderResourceView*>(e.object));
                note(p.header, "shader resource view");
                break;
            case kPacketSampler:
                set_sampler(c, p.stage, slot, static_cast<ID3D11SamplerState*>(e.object));
                note(p.header, "sampler");
                break;
            case kPacketBuffer:
                bind_buffer(c, p.header, p.stage, slot, static_cast<ID3D11Buffer*>(e.object), e.value);
                break;
            default:
                note(p.header, "unknown type: ignored", true);
                return S_OK;
        }
    }
    return S_OK;
}

}  // namespace g6shim::d3d
