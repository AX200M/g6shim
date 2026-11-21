// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "apu_memory.hpp"
#include "shape_config.hpp"

#include <windows.h>

#include <mutex>

namespace g6shim::acphal {

struct ShapeTable {
    ShapeArrays arrays{};
    ShapeCounts counts{};
    bool allocated = false;

    unsigned char* context(std::size_t kind, std::uint32_t index) const {
        if (!allocated || index >= counts[kind] || !arrays[kind]) return nullptr;
        return static_cast<unsigned char*>(arrays[kind]) + std::size_t(kContextBytes[kind]) * index;
    }
};

class ShapeContexts {
public:
    explicit ShapeContexts(ApuMemory& memory) : memory_(memory) {}
    ShapeContexts(const ShapeContexts&) = delete;
    ShapeContexts& operator=(const ShapeContexts&) = delete;

    HRESULT allocate(void* config) {
        if (!config) return E_INVALIDARG;
        const ShapeCounts counts = read_shape_counts(config);
        if (!shape_counts_valid(counts)) return E_INVALIDARG;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (table_.allocated) return kShapeContextsInUse;
        ShapeArrays arrays{};
        ShapeApuAddresses apu{};
        std::array<void*, kShapeKinds> blocks{};
        const auto fail = [&] {
            for (void* block : blocks)
                if (block) memory_.release(block);
            return E_OUTOFMEMORY;
        };
        for (std::size_t kind = 0; kind < kShapeKinds; ++kind) {
            if (kind == kShapeKindPcm) continue;
            std::size_t bytes = shape_array_bytes(kind, counts[kind]);
            if (kind == kShapeKindXma) bytes += shape_array_bytes(kShapeKindPcm, counts[kShapeKindPcm]);
            if (!bytes) continue;
            if (FAILED(memory_.allocate(static_cast<std::uint32_t>(bytes), kArrayAlignment[kind], &blocks[kind],
                                        &apu[kind])))
                return fail();
            arrays[kind] = counts[kind] ? blocks[kind] : nullptr;
        }
        if (counts[kShapeKindPcm]) {
            const std::size_t xma_bytes = shape_array_bytes(kShapeKindXma, counts[kShapeKindXma]);
            arrays[kShapeKindPcm] = static_cast<unsigned char*>(blocks[kShapeKindXma]) + xma_bytes;
            apu[kShapeKindPcm] = apu[kShapeKindXma] + static_cast<std::uint32_t>(xma_bytes);
            if (!counts[kShapeKindXma]) apu[kShapeKindXma] = 0;
        }
        table_ = {arrays, counts, true};
        blocks_ = blocks;
        write_shape_arrays(config, arrays, apu);
        return S_OK;
    }

    HRESULT release() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!table_.allocated) return kShapeContextsNotAllocated;
        for (void* block : blocks_)
            if (block) memory_.release(block);
        table_ = {};
        blocks_ = {};
        return S_OK;
    }

    template <class Fn>
    auto with_table(Fn&& fn) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return fn(table_);
    }

private:
    ApuMemory& memory_;
    std::recursive_mutex mutex_;
    ShapeTable table_;
    std::array<void*, kShapeKinds> blocks_{};
};

ShapeContexts& shape_contexts();

}  // namespace g6shim::acphal
