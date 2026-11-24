// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <g6shim/winrt/collections.hpp>
#include <g6shim/winrt/object.hpp>

#include <windows.h>
#include <inspectable.h>
#include <objidl.h>

#include <g6shim/log.hpp>

#include <atomic>
#include <vector>

namespace g6shim::winrt {

namespace {

constexpr char kEmptyMapView[] = "g6shim.EmptyMapView";

struct VectorView {
    const void* const* vtable;
    std::atomic<ULONG> refs{1};
    std::vector<IUnknown*> items;
};

HRESULT __stdcall QueryInterface(VectorView* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) || riid == __uuidof(IAgileObject)) {
        ++self->refs;
        *out = self;
        return S_OK;
    }
    G6SHIM_WARN("vector view: QueryInterface %s not supported", guid_text(riid).c_str());
    return E_NOINTERFACE;
}

ULONG __stdcall AddRef(VectorView* self) { return ++self->refs; }

ULONG __stdcall Release(VectorView* self) {
    const ULONG n = --self->refs;
    if (n == 0) {
        for (IUnknown* item : self->items) item->Release();
        delete self;
    }
    return n;
}

HRESULT __stdcall GetIids(VectorView*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

HRESULT __stdcall GetRuntimeClassName(VectorView*, HSTRING* name) {
    if (!name) return E_POINTER;
    *name = nullptr;
    return S_OK;
}

HRESULT __stdcall GetTrustLevel(VectorView*, TrustLevel* level) {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

HRESULT __stdcall GetAt(VectorView* self, UINT index, IUnknown** item) {
    if (!item) return E_POINTER;
    if (index >= self->items.size()) return E_BOUNDS;
    *item = self->items[index];
    (*item)->AddRef();
    return S_OK;
}

HRESULT __stdcall get_Size(VectorView* self, UINT* size) {
    if (!size) return E_POINTER;
    *size = static_cast<UINT>(self->items.size());
    return S_OK;
}

HRESULT __stdcall IndexOf(VectorView* self, IUnknown* value, UINT* index, BYTE* found) {
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = 0;
    for (UINT i = 0; i < self->items.size(); ++i)
        if (self->items[i] == value) {
            *index = i;
            *found = 1;
            break;
        }
    return S_OK;
}

HRESULT __stdcall GetMany(VectorView* self, UINT start, UINT capacity, IUnknown** items, UINT* actual) {
    if (!actual || (capacity && !items)) return E_POINTER;
    *actual = 0;
    for (UINT i = start; i < self->items.size() && *actual < capacity; ++i) {
        items[*actual] = self->items[i];
        items[*actual]->AddRef();
        ++*actual;
    }
    return S_OK;
}

const void* const* vector_vtable() {
    static const void* const table[] = {
        reinterpret_cast<const void*>(&QueryInterface), reinterpret_cast<const void*>(&AddRef),
        reinterpret_cast<const void*>(&Release),        reinterpret_cast<const void*>(&GetIids),
        reinterpret_cast<const void*>(&GetRuntimeClassName), reinterpret_cast<const void*>(&GetTrustLevel),
        reinterpret_cast<const void*>(&GetAt),          reinterpret_cast<const void*>(&get_Size),
        reinterpret_cast<const void*>(&IndexOf),        reinterpret_cast<const void*>(&GetMany),
    };
    return table;
}

}  // namespace

void* make_vector_view(const std::vector<void*>& items) {
    auto* v = new VectorView{vector_vtable()};
    for (void* item : items) {
        static_cast<IUnknown*>(item)->AddRef();
        v->items.push_back(static_cast<IUnknown*>(item));
    }
    return v;
}

void* make_empty_vector_view() { return make_vector_view({}); }

namespace {

HRESULT __stdcall Vector_GetView(VectorView* self, void** view) {
    if (!view) return E_POINTER;
    std::vector<void*> items(self->items.begin(), self->items.end());
    *view = make_vector_view(items);
    return S_OK;
}

HRESULT __stdcall Vector_SetAt(VectorView* self, UINT index, IUnknown* item) {
    if (index >= self->items.size()) return E_BOUNDS;
    if (item) item->AddRef();
    if (self->items[index]) self->items[index]->Release();
    self->items[index] = item;
    return S_OK;
}

HRESULT __stdcall Vector_InsertAt(VectorView* self, UINT index, IUnknown* item) {
    if (index > self->items.size()) return E_BOUNDS;
    if (item) item->AddRef();
    self->items.insert(self->items.begin() + index, item);
    return S_OK;
}

HRESULT __stdcall Vector_RemoveAt(VectorView* self, UINT index) {
    if (index >= self->items.size()) return E_BOUNDS;
    if (self->items[index]) self->items[index]->Release();
    self->items.erase(self->items.begin() + index);
    return S_OK;
}

HRESULT __stdcall Vector_Append(VectorView* self, IUnknown* item) {
    if (item) item->AddRef();
    self->items.push_back(item);
    return S_OK;
}

HRESULT __stdcall Vector_RemoveAtEnd(VectorView* self) {
    if (self->items.empty()) return E_BOUNDS;
    return Vector_RemoveAt(self, static_cast<UINT>(self->items.size() - 1));
}

HRESULT __stdcall Vector_Clear(VectorView* self) {
    for (IUnknown* item : self->items)
        if (item) item->Release();
    self->items.clear();
    return S_OK;
}

HRESULT __stdcall Vector_ReplaceAll(VectorView* self, UINT count, IUnknown** items) {
    if (count && !items) return E_POINTER;
    Vector_Clear(self);
    for (UINT i = 0; i < count; ++i) Vector_Append(self, items[i]);
    return S_OK;
}

}  // namespace

void* make_vector() {
    static const void* const table[] = {
        reinterpret_cast<const void*>(&QueryInterface),  reinterpret_cast<const void*>(&AddRef),
        reinterpret_cast<const void*>(&Release),         reinterpret_cast<const void*>(&GetIids),
        reinterpret_cast<const void*>(&GetRuntimeClassName), reinterpret_cast<const void*>(&GetTrustLevel),
        reinterpret_cast<const void*>(&GetAt),           reinterpret_cast<const void*>(&get_Size),
        reinterpret_cast<const void*>(&Vector_GetView),  reinterpret_cast<const void*>(&IndexOf),
        reinterpret_cast<const void*>(&Vector_SetAt),    reinterpret_cast<const void*>(&Vector_InsertAt),
        reinterpret_cast<const void*>(&Vector_RemoveAt), reinterpret_cast<const void*>(&Vector_Append),
        reinterpret_cast<const void*>(&Vector_RemoveAtEnd), reinterpret_cast<const void*>(&Vector_Clear),
        reinterpret_cast<const void*>(&GetMany),         reinterpret_cast<const void*>(&Vector_ReplaceAll),
    };
    return new VectorView{table};
}

namespace {

HRESULT __stdcall Map_Lookup(void*, void*, void** value) {
    if (value) *value = nullptr;
    return E_BOUNDS;
}

HRESULT __stdcall Map_get_Size(void*, UINT* size) {
    if (!size) return E_POINTER;
    *size = 0;
    return S_OK;
}

HRESULT __stdcall Map_HasKey(void*, void*, BYTE* found) {
    if (!found) return E_POINTER;
    *found = 0;
    return S_OK;
}

HRESULT __stdcall Map_Split(void*, void** first, void** second) {
    if (first) *first = nullptr;
    if (second) *second = nullptr;
    return S_OK;
}

}  // namespace

void* make_empty_map_view() {
    static const void* const* vtable = make_vtable({{0, reinterpret_cast<const void*>(&Map_Lookup)},
                                                    {1, reinterpret_cast<const void*>(&Map_get_Size)},
                                                    {2, reinterpret_cast<const void*>(&Map_HasKey)},
                                                    {3, reinterpret_cast<const void*>(&Map_Split)}});
    return make_object(kEmptyMapView, kNoIid, vtable);
}

}  // namespace g6shim::winrt
