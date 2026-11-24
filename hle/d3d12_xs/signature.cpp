// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "signature.hpp"

#include <g6shim/log.hpp>

#include <utility>

namespace g6shim::d3d12_xs::signature {

namespace {

const std::array<const void*, 24>& table();

struct Signature : Object {
    Signature(std::uint32_t record_stride, std::vector<std::array<std::uint32_t, 4>> records)
        : Object(table().data()), stride(record_stride), arguments(std::move(records)) {}

    std::uint32_t stride;
    std::vector<std::array<std::uint32_t, 4>> arguments;
};

HRESULT query(Object* object, const Iid* iid, Object** out) {
    return answer(kCommandSignature, iid, is_iid(iid, kIids) ? object : nullptr, out);
}

std::uint32_t release_signature(Object* object) { return release_boxed<Signature>(object); }

std::array<const void*, 24> make_functions() {
    std::array<const void*, 24> f = d3d12_object(missing_table<kCommandSignature, 24>());
    f[0] = reinterpret_cast<const void*>(&query);
    f[2] = reinterpret_cast<const void*>(&release_signature);
    f[7] = reinterpret_cast<const void*>(&parent_device);
    return f;
}

const std::array<const void*, 24>& table() {
    static const std::array<const void*, 24> table = make_functions();
    return table;
}

}  // namespace

HRESULT create(Object*, const Desc* desc, Object*, const Iid* iid, Object** out) {
    if (desc == nullptr) return E_INVALIDARG;
    if (desc->arguments == nullptr && desc->count > 0) return E_INVALIDARG;
    std::vector<std::array<std::uint32_t, 4>> arguments;
    if (desc->count > 0) arguments.assign(desc->arguments, desc->arguments + desc->count);
    G6SHIM_TRACE("d3d12_xs: command signature (stride %u, %u arguments)", desc->stride, desc->count);
    return hand_out(kCommandSignature, std::make_unique<Signature>(desc->stride, std::move(arguments)), iid, kIids,
                    out);
}

std::optional<Layout> layout(Object* object) {
    if (object == nullptr) return std::nullopt;
    const auto* signature = static_cast<const Signature*>(object);
    return Layout{signature->stride, &signature->arguments};
}

}  // namespace g6shim::d3d12_xs::signature
