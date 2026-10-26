#ifndef LIUKAI_WINDOWS_USER_PROFILE_ENABLE_HPP
#define LIUKAI_WINDOWS_USER_PROFILE_ENABLE_HPP

#include <windows.h>
#include <msctf.h>

namespace boshiamy::user_profile {

// Enables a registered TSF profile for the current user, without selecting it
// as the active/default profile. The manager is injected so callers can test
// the state machine without invoking a system-changing API.
template<class ProfileManager>
HRESULT ensure_enabled_for_current_user(ProfileManager& manager,
                                        REFCLSID clsid,
                                        LANGID language,
                                        REFGUID profile) {
    BOOL enabled = FALSE;
    HRESULT hr = manager.IsEnabledLanguageProfile(clsid, language, profile, &enabled);
    if (FAILED(hr)) return hr;
    if (enabled) return S_FALSE;

    hr = manager.EnableLanguageProfile(clsid, language, profile, TRUE);
    if (FAILED(hr)) return hr;

    enabled = FALSE;
    hr = manager.IsEnabledLanguageProfile(clsid, language, profile, &enabled);
    if (FAILED(hr)) return hr;
    return enabled ? S_OK : E_FAIL;
}

} // namespace boshiamy::user_profile

#endif
