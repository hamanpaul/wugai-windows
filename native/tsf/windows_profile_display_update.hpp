#pragma once

#include "profile_display_update.hpp"
#include "windows_registration.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace boshiamy::profile_display {

inline constexpr wchar_t kKnownOldDescription[] = L"嘸＋改 Liu-Kai";
inline constexpr WORD kBrandingIconResourceId = 101;

template<class RegisterProfile>
inline HRESULT register_profile_description(RegisterProfile&& call,
    const ProfileSnapshot& captured, const std::wstring& description) {
    const bool exact_identity =
        captured.profile_type == TF_PROFILETYPE_INPUTPROCESSOR &&
        captured.language == boshiamy::kProfileLanguage &&
        IsEqualCLSID(captured.service, boshiamy::kServiceClsid) &&
        IsEqualGUID(captured.profile, boshiamy::kProfileGuid);
    const bool known_description =
        (captured.api_description == kKnownOldDescription ||
         captured.api_description == kDisplayDescription) &&
        (description == kKnownOldDescription || description == kDisplayDescription);
    bool usable_icon_path = captured.registry.icon_file.size() < MAX_PATH &&
        captured.registry.icon_file.find(L'\0') == std::wstring::npos;
    if (usable_icon_path) {
        try {
            usable_icon_path = std::filesystem::path(captured.registry.icon_file).is_absolute();
        } catch (...) {
            usable_icon_path = false;
        }
    }
    // RegisterProfile exposes bEnabledByDefault and TF_RP flags, but no
    // parameters for the separate SubItemInSettingUI metadata. Fail closed
    // unless the owned profile's current registration uses the known default
    // values so this call cannot silently discard an unrepresented setting.
    const bool registration_shape =
        captured.api_description == captured.registry.description &&
        captured.registry.icon_index == 0 &&
        captured.registry.enabled_by_default == 1 &&
        captured.registry.hidden_in_settings == 0 &&
        captured.registry.subitem_in_settings == 0 &&
        captured.substitute_hkl == 0 && captured.hkl == 0 &&
        usable_icon_path;
    if (!exact_identity || !known_description || !registration_shape)
        return E_INVALIDARG;

    return call(captured.service, captured.language, captured.profile,
        description.c_str(), static_cast<ULONG>(description.size()),
        captured.registry.icon_file.c_str(),
        static_cast<ULONG>(captured.registry.icon_file.size()),
        captured.registry.icon_index,
        reinterpret_cast<HKL>(captured.substitute_hkl), 0,
        captured.registry.enabled_by_default != 0, 0);
}

namespace windows_detail {

template<class T>
class ComRef {
    T* value_ = nullptr;
public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() { if (value_) value_->Release(); }
    T* get() const noexcept { return value_; }
    T** put() noexcept { return &value_; }
    T* operator->() const noexcept { return value_; }
};

class RegistryKey {
    HKEY value_ = nullptr;
public:
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey() { if (value_) RegCloseKey(value_); }
    HKEY get() const noexcept { return value_; }
    HKEY* put() noexcept { return &value_; }
};

inline HRESULT read_reg_string(HKEY key, const wchar_t* name, std::wstring* value) {
    if (!value) return E_POINTER;
    DWORD type = 0, bytes = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 65536 ||
        bytes % sizeof(wchar_t)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    status = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(buffer.data()), &bytes);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    if (type != REG_SZ || bytes != buffer.size() * sizeof(wchar_t) ||
        buffer.back() != L'\0' ||
        std::find(buffer.begin(), buffer.end() - 1, L'\0') != buffer.end() - 1)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    value->assign(buffer.data(), buffer.size() - 1);
    return S_OK;
}

inline HRESULT read_reg_dword(HKEY key, const wchar_t* name, DWORD* value) {
    if (!value) return E_POINTER;
    DWORD type = 0, bytes = sizeof(DWORD), result = 0;
    const LONG status = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(&result), &bytes);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    if (type != REG_DWORD || bytes != sizeof(DWORD))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    *value = result;
    return S_OK;
}

inline std::wstring guid_text(REFGUID guid) {
    wchar_t value[40]{};
    if (!StringFromGUID2(guid, value, static_cast<int>(std::size(value)))) return {};
    return value;
}

inline std::wstring profile_key_path() {
    return L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + guid_text(boshiamy::kServiceClsid) +
        L"\\LanguageProfile\\0x00000404\\" + guid_text(boshiamy::kProfileGuid);
}

inline std::wstring com_key_path() {
    return L"Software\\Classes\\CLSID\\" + guid_text(boshiamy::kServiceClsid);
}

// Lifecycle installation needs the icon binding, not a complete display-edit
// snapshot. A freshly registered Windows profile can omit the settings UI
// DWORDs; requiring them here makes successful RegisterProfile look like a
// failed installation. Keep the full metadata reader below strict for edits.
inline HRESULT read_profile_icon_path(HKEY hive, const std::wstring& key_path,
                                     std::wstring* path) {
    if (!path) return E_POINTER;
    path->clear();
    RegistryKey key;
    const LONG status = RegOpenKeyExW(hive, key_path.c_str(), 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put());
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    std::wstring observed;
    const HRESULT hr = read_reg_string(key.get(), L"IconFile", &observed);
    if (hr != S_OK) return hr;
    if (observed.empty()) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    *path = std::move(observed);
    return S_OK;
}

inline HRESULT read_profile_metadata(ProfileMetadata* metadata) {
    if (!metadata) return E_POINTER;
    RegistryKey key;
    const LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, profile_key_path().c_str(),
        0, KEY_QUERY_VALUE, key.put());
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    HRESULT hr = read_reg_string(key.get(), L"Description", &metadata->description);
    if (FAILED(hr)) return hr;
    hr = read_reg_string(key.get(), L"IconFile", &metadata->icon_file);
    if (FAILED(hr)) return hr;
    hr = read_reg_dword(key.get(), L"IconIndex", &metadata->icon_index);
    if (FAILED(hr)) return hr;
    hr = read_reg_dword(key.get(), L"Enable", &metadata->enabled_by_default);
    if (FAILED(hr)) return hr;
    hr = read_reg_dword(key.get(), L"HiddenInSettingUI", &metadata->hidden_in_settings);
    if (FAILED(hr)) return hr;
    return read_reg_dword(key.get(), L"SubItemInSettingUI", &metadata->subitem_in_settings);
}

inline bool read_owned_com_server_path(std::wstring* path) {
    if (!path) return false;
    RegistryKey clsid, server;
    const std::wstring clsid_path = com_key_path();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, clsid_path.c_str(), 0, KEY_QUERY_VALUE,
                      clsid.put()) != ERROR_SUCCESS) return false;
    std::wstring owner;
    if (FAILED(read_reg_string(clsid.get(), L"LiuKaiOwner", &owner)) ||
        owner != L"LiuKai-TSF-v1")
        return false;
    if (RegOpenKeyExW(clsid.get(), L"InprocServer32", 0, KEY_QUERY_VALUE,
                      server.put()) != ERROR_SUCCESS) return false;
    std::wstring threading_model;
    if (FAILED(read_reg_string(server.get(), L"ThreadingModel", &threading_model)) ||
        threading_model != L"Apartment" || FAILED(read_reg_string(server.get(), nullptr, path)) ||
        path->empty()) return false;
    ComServerRegistration owner_check(HKEY_CURRENT_USER, boshiamy::kServiceClsid, *path);
    return owner_check.matches() == S_OK;
}

inline bool validate_brand_resource(const ProfileMetadata& metadata) {
    if (metadata.icon_file.empty() || metadata.icon_index != 0 ||
        metadata.icon_file.size() >= MAX_PATH ||
        metadata.icon_file.find(L'\0') != std::wstring::npos) return false;
    try {
        if (!std::filesystem::path(metadata.icon_file).is_absolute()) return false;
    } catch (...) { return false; }
    const DWORD attributes = GetFileAttributesW(metadata.icon_file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    HMODULE module = LoadLibraryExW(metadata.icon_file.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return false;
    // RT_GROUP_ICON can expand to an ANSI pointer in non-UNICODE builds even
    // when FindResourceW is selected. Keep both resource arguments wide.
    const bool valid = FindResourceW(module, MAKEINTRESOURCEW(kBrandingIconResourceId),
                                     MAKEINTRESOURCEW(14)) != nullptr;
    FreeLibrary(module);
    return valid;
}

inline bool guid_less(const GUID& a, const GUID& b) {
    if (a.Data1 != b.Data1) return a.Data1 < b.Data1;
    if (a.Data2 != b.Data2) return a.Data2 < b.Data2;
    if (a.Data3 != b.Data3) return a.Data3 < b.Data3;
    return std::lexicographical_compare(std::begin(a.Data4), std::end(a.Data4),
                                        std::begin(b.Data4), std::end(b.Data4));
}

} // namespace windows_detail

class WindowsProfileDisplayBackend final : public ProfileDisplayBackend {
    windows_detail::ComRef<ITfInputProcessorProfileMgr> manager_;
    windows_detail::ComRef<ITfInputProcessorProfiles> profiles_;
    windows_detail::ComRef<ITfCategoryMgr> categories_;
    std::wstring initial_owned_com_path_;
    bool initial_owned_com_path_valid_ = false;
    HRESULT ready_ = E_UNEXPECTED;

public:
    WindowsProfileDisplayBackend() {
        initial_owned_com_path_valid_ =
            windows_detail::read_owned_com_server_path(&initial_owned_com_path_);
        ready_ = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
            CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
            reinterpret_cast<void**>(manager_.put()));
        if (FAILED(ready_)) return;
        ready_ = manager_->QueryInterface(IID_ITfInputProcessorProfiles,
            reinterpret_cast<void**>(profiles_.put()));
        if (FAILED(ready_)) return;
        ready_ = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, reinterpret_cast<void**>(categories_.put()));
    }

    HRESULT ready() const noexcept { return ready_; }

    HRESULT read(ProfileSnapshot* out) override {
        if (!out) return E_POINTER;
        if (FAILED(ready_)) return ready_;

        ProfileSnapshot snapshot{};
        BSTR description = nullptr;
        HRESULT hr = profiles_->GetLanguageProfileDescription(boshiamy::kServiceClsid,
            boshiamy::kProfileLanguage, boshiamy::kProfileGuid, &description);
        if (hr != S_OK || !description) {
            if (description) SysFreeString(description);
            return hr == S_OK ? E_FAIL : hr;
        }
        snapshot.api_description.assign(description, SysStringLen(description));
        SysFreeString(description);

        TF_INPUTPROCESSORPROFILE profile{};
        hr = manager_->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, boshiamy::kProfileLanguage,
            boshiamy::kServiceClsid, boshiamy::kProfileGuid, nullptr, &profile);
        if (hr != S_OK) return hr;
        snapshot.profile_type = profile.dwProfileType;
        snapshot.language = profile.langid;
        snapshot.service = profile.clsid;
        snapshot.profile = profile.guidProfile;
        snapshot.category = profile.catid;
        snapshot.capabilities = profile.dwCaps;
        snapshot.flags = profile.dwFlags;
        snapshot.substitute_hkl = reinterpret_cast<uintptr_t>(profile.hklSubstitute);
        snapshot.hkl = reinterpret_cast<uintptr_t>(profile.hkl);

        hr = profiles_->IsEnabledLanguageProfile(boshiamy::kServiceClsid,
            boshiamy::kProfileLanguage, boshiamy::kProfileGuid, &snapshot.enabled);
        if (hr != S_OK) return hr;
        snapshot.active_result = profiles_->GetActiveLanguageProfile(boshiamy::kServiceClsid,
            &snapshot.active_language, &snapshot.active_profile);
        if (snapshot.active_result != S_OK && snapshot.active_result != S_FALSE)
            return snapshot.active_result;
        snapshot.default_result = profiles_->GetDefaultLanguageProfile(boshiamy::kProfileLanguage,
            GUID_TFCAT_TIP_KEYBOARD, &snapshot.default_service, &snapshot.default_profile);
        if (snapshot.default_result != S_OK && snapshot.default_result != S_FALSE)
            return snapshot.default_result;

        IEnumGUID* raw_categories = nullptr;
        hr = categories_->EnumCategoriesInItem(boshiamy::kServiceClsid, &raw_categories);
        if (hr != S_OK || !raw_categories) {
            if (raw_categories) raw_categories->Release();
            return hr == S_OK ? E_FAIL : hr;
        }
        windows_detail::ComRef<IEnumGUID> category_enum;
        *category_enum.put() = raw_categories;
        for (;;) {
            GUID category{};
            ULONG fetched = 0;
            hr = category_enum->Next(1, &category, &fetched);
            if (FAILED(hr)) return hr;
            if (fetched == 0) break;
            snapshot.categories.push_back(category);
        }
        std::sort(snapshot.categories.begin(), snapshot.categories.end(), windows_detail::guid_less);
        hr = windows_detail::read_profile_metadata(&snapshot.registry);
        if (FAILED(hr)) return hr;
        *out = std::move(snapshot);
        return S_OK;
    }

    // Used only by an owned-profile relocation transaction. Full runtime and
    // metadata readback belongs to profile_icon::relocate; this write itself
    // repeats the complete expected-state and owned-COM checks.
    HRESULT write_icon_path(const ProfileSnapshot& captured, const std::wstring& icon_path) {
        if (ready_ != S_OK) return ready_;
        std::wstring current_owned_com_path;
        if (!initial_owned_com_path_valid_ ||
            !windows_detail::read_owned_com_server_path(&current_owned_com_path) ||
            CompareStringOrdinal(initial_owned_com_path_.c_str(), -1,
                current_owned_com_path.c_str(), -1, TRUE) != CSTR_EQUAL) return E_PENDING;
        ProfileSnapshot current;
        HRESULT hr = read(&current);
        if (hr != S_OK) return hr;
        if (!detail::same_snapshot(current, captured))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        ProfileSnapshot target = captured;
        target.registry.icon_file = icon_path;
        if (!windows_detail::validate_brand_resource(captured.registry) ||
            !windows_detail::validate_brand_resource(target.registry))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return register_profile_description(
            [&](REFCLSID clsid, LANGID language, REFGUID profile, const WCHAR* text,
                ULONG text_length, const WCHAR* icon_file, ULONG icon_length, ULONG icon_index,
                HKL substitute, DWORD preferred_layout, BOOL enabled_by_default, DWORD flags) {
                return manager_->RegisterProfile(clsid, language, profile, text, text_length,
                    icon_file, icon_length, icon_index, substitute, preferred_layout,
                    enabled_by_default, flags);
            }, target, captured.api_description);
    }

    HRESULT write_description(const ProfileSnapshot& captured,
                              const std::wstring& description) override {
        if (FAILED(ready_)) return ready_;
        std::wstring current_owned_com_path;
        if (!initial_owned_com_path_valid_ ||
            !windows_detail::read_owned_com_server_path(&current_owned_com_path) ||
            CompareStringOrdinal(initial_owned_com_path_.c_str(), -1,
                current_owned_com_path.c_str(), -1, TRUE) != CSTR_EQUAL)
            return E_PENDING;
        ProfileSnapshot current{};
        HRESULT hr = read(&current);
        if (FAILED(hr)) return hr;
        if (!detail::same_snapshot(current, captured))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if (!windows_detail::validate_brand_resource(captured.registry))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return register_profile_description(
            [&](REFCLSID clsid, LANGID language, REFGUID profile, const WCHAR* text,
                ULONG text_length, const WCHAR* icon_file, ULONG icon_length, ULONG icon_index,
                HKL substitute, DWORD preferred_layout, BOOL enabled_by_default, DWORD flags) {
                return manager_->RegisterProfile(clsid, language, profile, text, text_length,
                    icon_file, icon_length, icon_index, substitute, preferred_layout,
                    enabled_by_default, flags);
            }, captured, description);
    }
};

inline bool owned_profile_ready(const std::wstring& com_server_path,
                                const ProfileSnapshot& snapshot) {
    if (com_server_path.empty() || !detail::usable_expected(snapshot)) return false;
    if (snapshot.profile_type != TF_PROFILETYPE_INPUTPROCESSOR ||
        snapshot.language != boshiamy::kProfileLanguage ||
        !IsEqualCLSID(snapshot.service, boshiamy::kServiceClsid) ||
        !IsEqualGUID(snapshot.profile, boshiamy::kProfileGuid) ||
        snapshot.substitute_hkl != 0 || snapshot.hkl != 0 ||
        snapshot.api_description != snapshot.registry.description ||
        (snapshot.api_description != kKnownOldDescription &&
         snapshot.api_description != kDisplayDescription) ||
        snapshot.registry.enabled_by_default != 1 || snapshot.registry.icon_index != 0 ||
        snapshot.registry.hidden_in_settings != 0 || snapshot.registry.subitem_in_settings != 0)
        return false;
    return windows_detail::validate_brand_resource(snapshot.registry);
}

} // namespace boshiamy::profile_display
