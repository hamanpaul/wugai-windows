#pragma once

#include "profile_display_snapshot.hpp"
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <utility>

namespace boshiamy::installer::profile_display_plan {

inline constexpr size_t kMaxStableModuleUnits = 32760;

namespace detail {
inline bool known_description(const std::wstring& value) {
    return value == boshiamy::profile_display::kKnownOldDescription ||
        value == boshiamy::profile_display::kDisplayDescription;
}

inline bool exact_identity(const profile_display_state::DisplaySnapshot& snapshot) {
    const auto& profile = snapshot.profile;
    return profile.profile_type == TF_PROFILETYPE_INPUTPROCESSOR &&
        profile.language == boshiamy::kProfileLanguage &&
        IsEqualCLSID(profile.service, boshiamy::kServiceClsid) &&
        IsEqualGUID(profile.profile, boshiamy::kProfileGuid);
}

inline bool valid_stable_module(const std::wstring& module) {
    if (module.empty() || module.size() > kMaxStableModuleUnits ||
        module.find(L'\0') != std::wstring::npos)
        return false;
    try {
        return std::filesystem::path(module).is_absolute();
    } catch (...) {
        return false;
    }
}

inline profile_display_state::resource_name::RawRegistryValue terminated_reg_sz(
    const std::wstring& value) {
    profile_display_state::resource_name::RawRegistryValue raw;
    raw.status = ERROR_SUCCESS;
    raw.type = REG_SZ;
    std::wstring terminated = value;
    terminated.push_back(L'\0');
    raw.bytes.resize(terminated.size() * sizeof(wchar_t));
    std::memcpy(raw.bytes.data(), terminated.data(), raw.bytes.size());
    return raw;
}

inline HRESULT invalid_target() { return E_INVALIDARG; }
} // namespace detail

// This is a pure target projection. It neither establishes ownership nor
// authorizes registry writes. The caller must provide same-hive/view authority
// and maintain the stable module and resource 201 lifetime. Keep `before` intact
// as the inverse snapshot; its raw capture values are not validated or repaired.
inline HRESULT make_target(
    const profile_display_state::DisplaySnapshot& before,
    const std::wstring& stable_module,
    profile_display_state::DisplaySnapshot* target) {
    if (!target) return E_POINTER;
    *target = {};
    try {
        if (!detail::exact_identity(before) ||
            !detail::known_description(before.profile.api_description) ||
            !detail::known_description(before.profile.registry.description) ||
            !detail::valid_stable_module(stable_module))
            return detail::invalid_target();

        auto planned = before;
        planned.profile.api_description = boshiamy::profile_display::kDisplayDescription;
        planned.profile.registry.description = boshiamy::profile_display::kDisplayDescription;
        const auto description = detail::terminated_reg_sz(
            boshiamy::profile_display::kDisplayDescription);
        planned.descriptions.registry64 = description;
        planned.descriptions.registry32 = description;

        const std::wstring display_description = L"@" + stable_module + L",-201";
        const auto display_binding = detail::terminated_reg_sz(display_description);
        planned.display_binding.registry64 = display_binding;
        planned.display_binding.registry32 = display_binding;

        *target = std::move(planned);
        return S_OK;
    } catch (const std::bad_alloc&) {
        *target = {};
        return E_OUTOFMEMORY;
    } catch (...) {
        *target = {};
        return detail::invalid_target();
    }
}

} // namespace boshiamy::installer::profile_display_plan
