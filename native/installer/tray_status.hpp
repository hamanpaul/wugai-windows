#ifndef LIUKAI_INSTALLER_TRAY_STATUS_HPP
#define LIUKAI_INSTALLER_TRAY_STATUS_HPP

#include "tsf_shared_store.hpp"

namespace boshiamy::installer {

enum class SupportState { unknown, unsupported, supported };

struct SystemTrayObservation {
    HRESULT profile_status = E_UNEXPECTED;
    bool profile_present = false;
    HRESULT profile_caps_status = E_UNEXPECTED;
    DWORD profile_caps = 0;
    HRESULT category_status = E_UNEXPECTED;
    bool category_present = false;
    SupportState support = SupportState::unknown;
};

inline SystemTrayObservation observe_system_tray(TsfSharedApi& api) {
    SystemTrayObservation result;
    result.profile_status = api.profile_present(&result.profile_present);
    result.category_status = api.category_present(Resource::system_tray_category,
        &result.category_present);

    // Absence is a known unsupported state. Do not call GetProfile for a
    // missing profile: S_FALSE there is expected status, not a status failure.
    if (result.profile_status == S_OK && !result.profile_present) {
        result.profile_caps_status = S_FALSE;
        result.support = SupportState::unsupported;
        return result;
    }
    if (result.profile_status != S_OK) return result;

    result.profile_caps_status = api.profile_capabilities(&result.profile_caps);
    if (result.profile_caps_status != S_OK || result.category_status != S_OK)
        return result;

    result.support = result.category_present &&
        (result.profile_caps & kInstallerProfileCapsSysTraySupport) != 0
        ? SupportState::supported : SupportState::unsupported;
    return result;
}

inline const char* support_state_json(SupportState state) {
    switch (state) {
    case SupportState::supported: return "true";
    case SupportState::unsupported: return "false";
    default: return "null";
    }
}

} // namespace boshiamy::installer
#endif
