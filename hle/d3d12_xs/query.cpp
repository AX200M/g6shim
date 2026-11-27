// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "query.hpp"

#include <g6shim/log.hpp>

namespace g6shim::d3d12_xs::query {

namespace {

const std::array<const void*, 24>& table();

struct Heap : Object {
    explicit Heap(const Desc& description) : Object(table().data()), desc(description) {}

    Desc desc;
};

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kQueryHeap, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_heap(Object* object) { return release_boxed<Heap>(object); }

std::array<const void*, 24> make_functions() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kQueryHeap, 24>());
    f[0] = reinterpret_cast<const void*>(&query);
    f[2] = reinterpret_cast<const void*>(&release_heap);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    return f;
}

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> table = make_functions();
    return table;
}

}  // namespace

HRESULT create(Object*, const Desc* desc, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    G6SHIM_TRACE("d3d12_xs: query heap (kind %u, %u queries, %u nodes)", desc->kind, desc->count, desc->nodes);
    return hand_out(kQueryHeap, std::make_unique<Heap>(*desc), iid, kIids, out);
}

}  // namespace g6shim::d3d12_xs::query
