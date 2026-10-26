#ifndef BOSHIAMY_TSF_ACTIVATION_DIAGNOSTICS_HPP
#define BOSHIAMY_TSF_ACTIVATION_DIAGNOSTICS_HPP

#include <windows.h>
#include <msctf.h>
#include <oleauto.h>

// MinGW-w64's msctf.h does not declare ITfTextInputProcessorEx. The interface
// UUID and ABI below match the Windows SDK declaration. Keep this guarded so
// Windows SDK builds use the platform declaration.
#ifndef __ITfTextInputProcessorEx_INTERFACE_DEFINED__
#define __ITfTextInputProcessorEx_INTERFACE_DEFINED__
MIDL_INTERFACE("6e4e2102-f9cd-433d-b496-303ce03a6507")
ITfTextInputProcessorEx : public ITfTextInputProcessor {
    virtual HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr *thread_manager,
        TfClientId client_id, DWORD flags) = 0;
};
#endif

namespace boshiamy::tsf_activation {

// The MinGW headers used by this repository declare the interface only via
// the fallback above and do not provide __uuidof metadata for it. This IID is
// from Microsoft's Windows SDK msctf.idl declaration.
inline constexpr IID kTextInputProcessorExIid = {
    0x6e4e2102, 0xf9cd, 0x433d, {0xb4, 0x96, 0x30, 0x3c, 0xe0, 0x3a, 0x65, 0x07}
};

enum class EntryPoint { LegacyActivate, ActivateEx };

// ITfTextInputProcessorEx derives from ITfTextInputProcessor in the Windows
// SDK IDL. This helper shares the legacy and extended activation work while
// preserving the entry point and flags received from the host.
// Owner must implement:
//   HRESULT activate_common(ITfThreadMgr*, TfClientId, EntryPoint, DWORD);
//   HRESULT deactivate_common();
// It deliberately forwards exactly the host's flags and invents no capability.
template<class Owner>
class ActivationDispatch {
    Owner *owner_;
public:
    explicit ActivationDispatch(Owner *owner) noexcept : owner_(owner) {}
    HRESULT legacy_activate(ITfThreadMgr *manager, TfClientId client) const {
        return owner_ ? owner_->activate_common(manager, client, EntryPoint::LegacyActivate, 0) : E_UNEXPECTED;
    }
    HRESULT extended_activate(ITfThreadMgr *manager, TfClientId client, DWORD flags) const {
        return owner_ ? owner_->activate_common(manager, client, EntryPoint::ActivateEx, flags) : E_UNEXPECTED;
    }
    HRESULT deactivate() const {
        return owner_ ? owner_->deactivate_common() : E_UNEXPECTED;
    }
};

struct ActiveFlags {
    HRESULT query_result = E_UNEXPECTED;
    HRESULT read_result = E_UNEXPECTED;
    DWORD value = 0;
    bool has_value = false;
};

inline ActiveFlags read_active_flags(ITfThreadMgr *manager) noexcept {
    ActiveFlags result;
    if (!manager) { result.query_result = E_INVALIDARG; return result; }
    ITfThreadMgrEx *extended = nullptr;
    result.query_result = manager->QueryInterface(IID_ITfThreadMgrEx,
        reinterpret_cast<void **>(&extended));
    if (FAILED(result.query_result)) return result;
    result.read_result = extended->GetActiveFlags(&result.value);
    result.has_value = SUCCEEDED(result.read_result);
    extended->Release();
    return result;
}

struct ContextFlag {
    HRESULT enumerate_result = E_UNEXPECTED;
    HRESULT value_result = E_UNEXPECTED;
    bool present = false;
    bool known = false;
    bool value = false;
};

struct ContextGates {
    HRESULT manager_query_result = E_UNEXPECTED;
    HRESULT enumeration_result = E_UNEXPECTED;
    ContextFlag keyboard_disabled;
    ContextFlag empty_context;
};

inline ContextFlag read_existing_context_flag(ITfCompartmentMgr *manager,
    IEnumGUID *enumerator, REFGUID id) noexcept {
    ContextFlag result;
    if (!manager || !enumerator) { result.enumerate_result = E_INVALIDARG; return result; }
    HRESULT hr = enumerator->Reset();
    if (hr != S_OK) { result.enumerate_result = hr; return result; }
    for (;;) {
        GUID current{};
        ULONG fetched = 0;
        hr = enumerator->Next(1, &current, &fetched);
        if (hr == S_FALSE && fetched == 0) { result.enumerate_result = S_FALSE; return result; }
        if (hr != S_OK || fetched != 1) {
            result.enumerate_result = hr == S_OK ? E_UNEXPECTED : hr;
            return result;
        }
        if (IsEqualGUID(current, id)) { result.present = true; result.enumerate_result = S_OK; break; }
    }

    // GetCompartment creates a compartment when it does not exist. Enumerating
    // first makes this a read-only diagnostic for an absent host signal.
    ITfCompartment *compartment = nullptr;
    hr = manager->GetCompartment(id, &compartment);
    if (hr != S_OK || !compartment) {
        result.value_result = hr == S_OK ? E_UNEXPECTED : hr;
        if (compartment) compartment->Release();
        return result;
    }
    VARIANT value;
    VariantInit(&value);
    result.value_result = compartment->GetValue(&value);
    if (result.value_result == S_OK) {
        if (value.vt == VT_I4) {
            result.known = true;
            result.value = value.lVal != 0;
        } else if (value.vt == VT_EMPTY) {
            result.value_result = S_FALSE;
        } else {
            result.value_result = DISP_E_TYPEMISMATCH;
        }
    }
    VariantClear(&value);
    compartment->Release();
    return result;
}

inline ContextGates read_context_gates(ITfContext *context) noexcept {
    ContextGates result;
    if (!context) { result.manager_query_result = E_INVALIDARG; return result; }
    ITfCompartmentMgr *manager = nullptr;
    result.manager_query_result = context->QueryInterface(IID_ITfCompartmentMgr,
        reinterpret_cast<void **>(&manager));
    if (result.manager_query_result != S_OK || !manager) {
        if (result.manager_query_result == S_OK) result.manager_query_result = E_UNEXPECTED;
        if (manager) manager->Release();
        return result;
    }
    IEnumGUID *enumerator = nullptr;
    result.enumeration_result = manager->EnumCompartments(&enumerator);
    if (result.enumeration_result == S_OK && enumerator) {
        result.keyboard_disabled = read_existing_context_flag(manager, enumerator,
            GUID_COMPARTMENT_KEYBOARD_DISABLED);
        result.empty_context = read_existing_context_flag(manager, enumerator,
            GUID_COMPARTMENT_EMPTYCONTEXT);
        enumerator->Release();
    } else {
        if (result.enumeration_result == S_OK) result.enumeration_result = E_UNEXPECTED;
        if (enumerator) enumerator->Release();
    }
    manager->Release();
    return result;
}

enum class ContextDisposition { Blocked, Clear, Unknown };

inline ContextDisposition classify_context(const ContextGates &gates) noexcept {
    if ((gates.keyboard_disabled.known && gates.keyboard_disabled.value) ||
        (gates.empty_context.known && gates.empty_context.value))
        return ContextDisposition::Blocked;
    if (gates.keyboard_disabled.known && gates.empty_context.known)
        return ContextDisposition::Clear;
    return ContextDisposition::Unknown;
}

} // namespace boshiamy::tsf_activation

#endif
