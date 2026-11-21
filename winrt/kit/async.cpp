// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <g6shim/winrt/async.hpp>
#include <g6shim/winrt/object.hpp>

#include <windows.h>
#include <inspectable.h>
#include <objidl.h>

#include <g6shim/log.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace g6shim::winrt {

namespace {

constexpr GUID kIAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr int kAsyncStatusCompleted = 1;
constexpr int kAsyncStatusError = 3;
constexpr UINT kAsyncId = 1;

struct AsyncOperation {
    const void* const* operation_vtable;
    const void* const* info_vtable;
    std::atomic<ULONG> refs{1};
    IUnknown* result = nullptr;
    IUnknown* handler = nullptr;
    HRESULT error = S_OK;
};

AsyncOperation* from_info(void* info) {
    return reinterpret_cast<AsyncOperation*>(static_cast<char*>(info) - offsetof(AsyncOperation, info_vtable));
}

int status_of(const AsyncOperation* op) { return op->error == S_OK ? kAsyncStatusCompleted : kAsyncStatusError; }

HRESULT __stdcall QueryInterface(AsyncOperation* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    ++self->refs;
    if (riid == kIAsyncInfo) {
        *out = &self->info_vtable;
        return S_OK;
    }
    if (riid != __uuidof(IUnknown) && riid != __uuidof(IInspectable) && riid != __uuidof(IAgileObject)) {
        const std::string iid = guid_text(riid);
        log_once("async" + iid, "async operation: QueryInterface %s -> the operation", iid.c_str());
    }
    *out = self;
    return S_OK;
}

ULONG __stdcall AddRef(AsyncOperation* self) { return ++self->refs; }

ULONG __stdcall Release(AsyncOperation* self) {
    const ULONG n = --self->refs;
    if (n == 0) {
        if (self->result) self->result->Release();
        if (self->handler) self->handler->Release();
        delete self;
    }
    return n;
}

HRESULT __stdcall GetIids(void*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

HRESULT __stdcall GetRuntimeClassName(void*, HSTRING* name) {
    if (!name) return E_POINTER;
    *name = nullptr;
    return S_OK;
}

HRESULT __stdcall GetTrustLevel(void*, TrustLevel* level) {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

HRESULT __stdcall put_Completed(AsyncOperation* self, IUnknown* handler) {
    if (!handler) return E_INVALIDARG;
    handler->AddRef();
    if (self->handler) self->handler->Release();
    self->handler = handler;
    return vcall(handler, kDelegateInvokeSlot, static_cast<void*>(self), status_of(self));
}

HRESULT __stdcall get_Completed(AsyncOperation* self, IUnknown** handler) {
    if (!handler) return E_POINTER;
    *handler = self->handler;
    if (*handler) (*handler)->AddRef();
    return S_OK;
}

HRESULT __stdcall GetResults(AsyncOperation* self, IUnknown** result) {
    if (!result) return E_POINTER;
    if (self->error != S_OK) {
        *result = nullptr;
        return self->error;
    }
    *result = self->result;
    if (*result) (*result)->AddRef();
    return S_OK;
}

HRESULT __stdcall GetActionResults(AsyncOperation*) { return S_OK; }

HRESULT __stdcall InfoQueryInterface(void* info, REFIID riid, void** out) {
    return QueryInterface(from_info(info), riid, out);
}
ULONG __stdcall InfoAddRef(void* info) { return AddRef(from_info(info)); }
ULONG __stdcall InfoRelease(void* info) { return Release(from_info(info)); }

HRESULT __stdcall get_Id(void*, UINT* id) {
    if (!id) return E_POINTER;
    *id = kAsyncId;
    return S_OK;
}

HRESULT __stdcall get_Status(void* info, int* status) {
    if (!status) return E_POINTER;
    *status = status_of(from_info(info));
    return S_OK;
}

HRESULT __stdcall get_ErrorCode(void* info, HRESULT* code) {
    if (!code) return E_POINTER;
    *code = from_info(info)->error;
    return S_OK;
}

HRESULT __stdcall Cancel(void*) { return S_OK; }
HRESULT __stdcall Close(void*) { return S_OK; }

const void* const kInfo[] = {fn(&InfoQueryInterface), fn(&InfoAddRef), fn(&InfoRelease),
                             fn(&GetIids),            fn(&GetRuntimeClassName), fn(&GetTrustLevel),
                             fn(&get_Id),             fn(&get_Status), fn(&get_ErrorCode),
                             fn(&Cancel),             fn(&Close)};

}  // namespace

void* make_completed_async(void* result) {
    static const void* const operation[] = {fn(&QueryInterface), fn(&AddRef),        fn(&Release),
                                            fn(&GetIids),        fn(&GetRuntimeClassName), fn(&GetTrustLevel),
                                            fn(&put_Completed),  fn(&get_Completed), fn(&GetResults)};
    auto* op = new AsyncOperation{operation, kInfo};
    if (result) {
        static_cast<IUnknown*>(result)->AddRef();
        op->result = static_cast<IUnknown*>(result);
    }
    return op;
}

void* make_failed_async(long error) {
    auto* op = static_cast<AsyncOperation*>(make_completed_async(nullptr));
    op->error = error;
    return op;
}

void* make_completed_action() {
    static const void* const action[] = {fn(&QueryInterface), fn(&AddRef),        fn(&Release),
                                         fn(&GetIids),        fn(&GetRuntimeClassName), fn(&GetTrustLevel),
                                         fn(&put_Completed),  fn(&get_Completed), fn(&GetActionResults)};
    return new AsyncOperation{action, kInfo};
}

}  // namespace g6shim::winrt
