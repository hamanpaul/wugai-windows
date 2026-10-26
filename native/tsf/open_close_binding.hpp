#ifndef BOSHIAMY_OPEN_CLOSE_BINDING_HPP
#define BOSHIAMY_OPEN_CLOSE_BINDING_HPP

#include <windows.h>
#include <msctf.h>
#include <olectl.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace boshiamy {

// The owner and callback run on the TSF STA; this object does not marshal callback calls.
class OpenCloseBinding {
    class Sink final : public ITfCompartmentEventSink {
        std::shared_ptr<void> lifetime_;
        std::atomic<ULONG> refs_{1};
        std::recursive_mutex mutex_;
        std::function<void()> callback_;
        GUID watched_guid_;
    public:
        Sink(std::function<void()> callback, std::shared_ptr<void> lifetime, REFGUID watched_guid)
            : lifetime_(std::move(lifetime)), callback_(std::move(callback)), watched_guid_(watched_guid) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfCompartmentEventSink))
                *out = static_cast<ITfCompartmentEventSink*>(this);
            else return E_NOINTERFACE;
            AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (!n) delete this;
            return n;
        }
        HRESULT STDMETHODCALLTYPE OnChange(REFGUID guid) override {
            AddRef();
            HRESULT result = S_OK;
            if (IsEqualGUID(guid, watched_guid_)) {
                try {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    auto callback = callback_;
                    if (callback) callback();
                }
                catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
                catch (...) { result = E_FAIL; }
            }
            Release();
            return result;
        }
        void disable() {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            callback_ = {};
        }
    };

    ITfSource* source_ = nullptr;
    ITfCompartment* compartment_ = nullptr;
    Sink* sink_ = nullptr;
    DWORD cookie_ = TF_INVALID_COOKIE;
    TfClientId client_ = 0;

    void release_subscription() {
        if (sink_) { sink_->disable(); sink_->Release(); sink_ = nullptr; }
        if (source_) { source_->Release(); source_ = nullptr; }
        if (compartment_) { compartment_->Release(); compartment_ = nullptr; }
        cookie_ = TF_INVALID_COOKIE;
        client_ = 0;
    }

public:
    OpenCloseBinding() = default;
    OpenCloseBinding(const OpenCloseBinding&) = delete;
    OpenCloseBinding& operator=(const OpenCloseBinding&) = delete;
    ~OpenCloseBinding() {
        if (sink_) sink_->disable();
        if (source_ && cookie_ != TF_INVALID_COOKIE) source_->UnadviseSink(cookie_);
        release_subscription();
    }

    HRESULT attach(ITfThreadMgr* manager, TfClientId client, std::function<void()> callback,
                   std::shared_ptr<void> lifetime = {},
                   REFGUID guid = GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) {
        if (!manager) return E_INVALIDARG;
        if (source_ || sink_ || compartment_) {
            HRESULT hr = detach();
            if (FAILED(hr)) return hr;
        }
        ITfCompartmentMgr* manager_compartments = nullptr;
        HRESULT hr = manager->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void**>(&manager_compartments));
        if (FAILED(hr)) return hr;
        ITfCompartment* compartment = nullptr;
        hr = manager_compartments->GetCompartment(guid, &compartment);
        manager_compartments->Release();
        if (FAILED(hr)) return hr;
        hr = attach(compartment, client, std::move(callback), std::move(lifetime), guid);
        compartment->Release();
        return hr;
    }

    HRESULT attach(ITfCompartment* compartment, TfClientId client, std::function<void()> callback,
                   std::shared_ptr<void> lifetime = {},
                   REFGUID guid = GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) {
        if (!compartment) return E_INVALIDARG;
        if (source_ || sink_ || compartment_) {
            HRESULT hr = detach();
            if (FAILED(hr)) return hr;
        }
        ITfSource* source = nullptr;
        HRESULT hr = compartment->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
        if (FAILED(hr)) return hr;
        auto* sink = new (std::nothrow) Sink(std::move(callback), std::move(lifetime), guid);
        if (!sink) { source->Release(); return E_OUTOFMEMORY; }
        DWORD cookie = TF_INVALID_COOKIE;
        hr = source->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(sink), &cookie);
        if (FAILED(hr)) { sink->disable(); sink->Release(); source->Release(); return hr; }
        compartment->AddRef();
        source_ = source; compartment_ = compartment; sink_ = sink; cookie_ = cookie; client_ = client;
        return S_OK;
    }

    HRESULT read_long(LONG& result) const {
        if (!compartment_) return E_UNEXPECTED;
        VARIANT value; VariantInit(&value);
        HRESULT hr = compartment_->GetValue(&value);
        if (hr == S_FALSE) { VariantClear(&value); return S_FALSE; }
        if (FAILED(hr)) { VariantClear(&value); return hr; }
        if (value.vt == VT_EMPTY) { VariantClear(&value); return S_FALSE; }
        if (value.vt != VT_I4) { VariantClear(&value); return DISP_E_TYPEMISMATCH; }
        LONG read_result = value.lVal;
        VariantClear(&value);
        result = read_result;
        return S_OK;
    }

    HRESULT write_long(LONG value) {
        if (!compartment_) return E_UNEXPECTED;
        LONG current = 0;
        HRESULT hr = read_long(current);
        if (hr == S_OK && current == value) return S_FALSE;
        if (hr != S_OK && hr != S_FALSE) return hr;
        VARIANT new_value; VariantInit(&new_value); new_value.vt = VT_I4; new_value.lVal = value;
        return compartment_->SetValue(client_, &new_value);
    }

    HRESULT update_bits(DWORD mask, DWORD bits) {
        if (!compartment_) return E_UNEXPECTED;
        LONG current = 0;
        HRESULT hr = read_long(current);
        if (hr == S_FALSE) current = 0;
        else if (hr != S_OK) return hr;
        DWORD raw = static_cast<DWORD>(current);
        DWORD updated_raw = (raw & ~mask) | (bits & mask);
        if (updated_raw == raw) return S_FALSE;
        LONG updated;
        std::memcpy(&updated, &updated_raw, sizeof(updated));
        VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = updated;
        return compartment_->SetValue(client_, &value);
    }

    HRESULT read(bool& open) const {
        LONG value = 0;
        HRESULT hr = read_long(value);
        if (hr != S_OK) return hr;
        open = value != 0;
        return S_OK;
    }

    HRESULT write(bool open) {
        if (!compartment_) return E_UNEXPECTED;
        LONG current = 0;
        HRESULT hr = read_long(current);
        if (hr == S_OK && (current != 0) == open) return S_FALSE;
        if (hr != S_OK && hr != S_FALSE) return hr;
        VARIANT value; VariantInit(&value); value.vt = VT_I4; value.lVal = open ? 1 : 0;
        return compartment_->SetValue(client_, &value);
    }

    HRESULT detach() {
        if (!source_ && !sink_ && !compartment_) return S_FALSE;
        if (sink_) sink_->disable();
        if (source_ && cookie_ != TF_INVALID_COOKIE) {
            HRESULT hr = source_->UnadviseSink(cookie_);
            if (FAILED(hr) && hr != CONNECT_E_NOCONNECTION) return hr;
        }
        release_subscription();
        return S_OK;
    }

    bool subscribed() const { return source_ != nullptr && sink_ != nullptr && cookie_ != TF_INVALID_COOKIE; }
};

}
#endif
