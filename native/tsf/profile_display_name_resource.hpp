#pragma once

#include "windows_profile_display_update.hpp"

#include <filesystem>
#include <cstring>
#include <vector>

namespace boshiamy::profile_display::resource_name {

// MinGW's msctf.h declares ITfInputProcessorProfiles but omits the Ex type.
// Keep the SDK inheritance so this method occupies the SDK's final vtable
// slot after the full base interface, not slot 3 after IUnknown.
struct ITfInputProcessorProfilesExCompat : ITfInputProcessorProfiles {
    virtual HRESULT STDMETHODCALLTYPE SetLanguageProfileDisplayName(
        REFCLSID service, LANGID language, REFGUID profile, const WCHAR* file,
        ULONG file_length, ULONG resource_id) = 0;
};

inline constexpr IID kProfilesExIid =
    {0x892f230f, 0xfe00, 0x4a41, {0xa9, 0x8e, 0xfc, 0xd6, 0xde, 0x0d, 0x35, 0xef}};

inline constexpr ULONG kDisplayNameResourceId = 201;

struct RawRegistryValue {
    LONG status = ERROR_FILE_NOT_FOUND;
    DWORD type = 0;
    std::vector<BYTE> bytes;
};

struct DisplayNameBinding {
    RawRegistryValue registry64;
    RawRegistryValue registry32;
};

inline bool same_value(const RawRegistryValue& a, const RawRegistryValue& b) {
    return a.status == b.status && a.type == b.type && a.bytes == b.bytes;
}

inline bool same_binding(const DisplayNameBinding& a, const DisplayNameBinding& b) {
    return same_value(a.registry64, b.registry64) &&
           same_value(a.registry32, b.registry32);
}

inline bool valid_raw_value(const RawRegistryValue& value, bool require_present) {
    if (value.status == ERROR_FILE_NOT_FOUND)
        return !require_present && value.type == 0 && value.bytes.empty();
    if (value.status != ERROR_SUCCESS || value.type != REG_SZ ||
        value.bytes.size() < sizeof(wchar_t) ||
        value.bytes.size() % sizeof(wchar_t) != 0) return false;
    const size_t count = value.bytes.size() / sizeof(wchar_t);
    std::wstring chars(count, L'\0');
    std::memcpy(chars.data(), value.bytes.data(), value.bytes.size());
    return chars.back() == L'\0' && chars.find(L'\0') == chars.size() - 1;
}

inline bool raw_value_text(const RawRegistryValue& value, std::wstring* text) {
    if (!text || !valid_raw_value(value, true)) return false;
    std::wstring raw(value.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(raw.data(), value.bytes.data(), value.bytes.size());
    raw.resize(raw.size() - 1);
    if (raw.empty() || raw.front() != L'@') return false;
    *text = std::move(raw);
    return true;
}

class Backend {
public:
    virtual ~Backend() = default;
    virtual HRESULT read_profile(ProfileSnapshot* snapshot) = 0;
    virtual HRESULT read_display_binding(DisplayNameBinding* binding) = 0;
    virtual HRESULT read_owned_module_path(std::wstring* path) = 0;
    virtual HRESULT validate_resource_201(const std::wstring& module_path) = 0;
    virtual HRESULT set_display_name_resource_201(const std::wstring& module_path) = 0;
    virtual HRESULT resolve_display_binding(const DisplayNameBinding& binding,
                                            std::wstring* resolved) = 0;
};

struct UpdateResult {
    HRESULT operation = E_NOTIMPL;
    HRESULT validation = S_OK;
    HRESULT readback = E_PENDING;
    bool setter_attempted = false;
    bool state_changed = false;
    bool readback_verified = false;
    std::wstring resolved_display_name;
};

inline bool exact_owned_identity(const ProfileSnapshot& snapshot) {
    return snapshot.profile_type == TF_PROFILETYPE_INPUTPROCESSOR &&
        snapshot.language == boshiamy::kProfileLanguage &&
        IsEqualCLSID(snapshot.service, boshiamy::kServiceClsid) &&
        IsEqualGUID(snapshot.profile, boshiamy::kProfileGuid);
}

inline bool known_description(const std::wstring& value) {
    return value == kKnownOldDescription || value == kDisplayDescription;
}

inline bool usable_resource_profile(const ProfileSnapshot& snapshot) {
    return exact_owned_identity(snapshot) && !IsEqualGUID(snapshot.category, GUID_NULL) &&
        !snapshot.registry.icon_file.empty() && snapshot.registry.icon_index == 0 &&
        snapshot.registry.enabled_by_default == 1 && snapshot.registry.hidden_in_settings == 0 &&
        snapshot.registry.subitem_in_settings == 0 && snapshot.substitute_hkl == 0 &&
        snapshot.hkl == 0 && known_description(snapshot.api_description) &&
        known_description(snapshot.registry.description);
}

inline bool stable_display_only_change(const ProfileSnapshot& before,
                                       const ProfileSnapshot& after) {
    return detail::same_except_descriptions(before, after) &&
        detail::only_our_description_write(before, after, kDisplayDescription);
}

inline HRESULT validate_binding(const DisplayNameBinding& binding, bool require_present) {
    if (!valid_raw_value(binding.registry64, require_present) ||
        !valid_raw_value(binding.registry32, require_present))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (!same_value(binding.registry64, binding.registry32))
        return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    return S_OK;
}

inline bool exact_path_equal(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline bool binding_targets_resource_201(const DisplayNameBinding& binding,
                                         const std::wstring& module_path) {
    std::wstring text64, text32;
    if (!raw_value_text(binding.registry64, &text64) ||
        !raw_value_text(binding.registry32, &text32) || text64 != text32) return false;
    const std::wstring expected = L"@" + module_path + L",-" +
        std::to_wstring(kDisplayNameResourceId);
    return text64 == expected;
}

inline UpdateResult update_display_name_resource(Backend& backend,
    const ProfileSnapshot& expected_profile, const DisplayNameBinding& expected_binding,
    const std::wstring& module_path) {
    UpdateResult result{};
    if (!usable_resource_profile(expected_profile) || module_path.empty()) {
        result.operation = E_INVALIDARG;
        result.validation = result.operation;
        return result;
    }
    try {
        if (!std::filesystem::path(module_path).is_absolute()) {
            result.operation = E_INVALIDARG;
            result.validation = result.operation;
            return result;
        }
    } catch (...) {
        result.operation = E_INVALIDARG;
        result.validation = result.operation;
        return result;
    }
    result.validation = validate_binding(expected_binding, false);
    if (result.validation != S_OK) {
        result.operation = result.validation;
        return result;
    }

    std::wstring owned_path;
    HRESULT hr = backend.read_owned_module_path(&owned_path);
    if (hr != S_OK || !exact_path_equal(module_path, owned_path)) {
        result.operation = hr == S_OK ? HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) : hr;
        result.validation = result.operation;
        return result;
    }
    hr = backend.validate_resource_201(module_path);
    if (hr != S_OK) {
        result.operation = hr;
        result.validation = result.operation;
        return result;
    }

    ProfileSnapshot current_profile{};
    DisplayNameBinding current_binding{};
    hr = backend.read_profile(&current_profile);
    if (hr != S_OK) {
        result.operation = hr;
        result.validation = hr;
        return result;
    }
    hr = backend.read_display_binding(&current_binding);
    if (hr != S_OK) {
        result.operation = hr;
        result.validation = hr;
        return result;
    }
    hr = validate_binding(current_binding, false);
    if (hr != S_OK || !detail::same_snapshot(current_profile, expected_profile) ||
        !same_binding(current_binding, expected_binding)) {
        result.operation = hr != S_OK ? hr : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        result.validation = result.operation;
        return result;
    }
    owned_path.clear();
    hr = backend.read_owned_module_path(&owned_path);
    if (hr != S_OK || !exact_path_equal(module_path, owned_path)) {
        result.operation = hr == S_OK ? HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) : hr;
        result.validation = result.operation;
        return result;
    }

    result.setter_attempted = true;
    const HRESULT setter_status = backend.set_display_name_resource_201(module_path);

    ProfileSnapshot after_profile{};
    DisplayNameBinding after_binding{};
    const HRESULT profile_status = backend.read_profile(&after_profile);
    const HRESULT binding_status = backend.read_display_binding(&after_binding);
    owned_path.clear();
    const HRESULT owner_status = backend.read_owned_module_path(&owned_path);
    if (profile_status != S_OK || binding_status != S_OK || owner_status != S_OK) {
        result.readback = profile_status != S_OK ? profile_status :
            (binding_status != S_OK ? binding_status : owner_status);
        result.operation = setter_status == S_OK ? result.readback : setter_status;
        return result;
    }

    result.state_changed = !detail::same_snapshot(expected_profile, after_profile) ||
        !same_binding(expected_binding, after_binding);
    result.readback = validate_binding(after_binding, true);
    const bool safe_profile = stable_display_only_change(expected_profile, after_profile);
    std::wstring resolved;
    const HRESULT resolve_status = result.readback == S_OK
        ? backend.resolve_display_binding(after_binding, &resolved) : result.readback;
    result.resolved_display_name = resolved;
    const bool desired_binding = binding_targets_resource_201(after_binding, module_path) &&
        resolve_status == S_OK && resolved == kDisplayDescription;
    const bool owner_unchanged = exact_path_equal(module_path, owned_path);
    const bool verified = safe_profile && desired_binding && owner_unchanged;
    result.readback_verified = verified;
    if (setter_status != S_OK) {
        result.operation = setter_status;
        return result;
    }
    if (!verified) {
        result.operation = resolve_status != S_OK ? resolve_status :
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return result;
    }
    result.operation = S_OK;
    result.readback = S_OK;
    return result;
}

} // namespace boshiamy::profile_display::resource_name
