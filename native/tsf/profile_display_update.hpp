#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace boshiamy::profile_display {

inline constexpr wchar_t kDisplayDescription[] = L"嘸．改";

struct ProfileMetadata {
    std::wstring description;
    std::wstring icon_file;
    DWORD icon_index = 0;
    DWORD enabled_by_default = 0;
    DWORD hidden_in_settings = 0;
    DWORD subitem_in_settings = 0;
};

// Captures both display strings and every profile/runtime field that a
// description-only update must leave unchanged.
struct ProfileSnapshot {
    DWORD profile_type = 0;
    LANGID language = 0;
    CLSID service = GUID_NULL;
    GUID profile = GUID_NULL;
    GUID category = GUID_NULL;
    DWORD capabilities = 0;
    DWORD flags = 0;
    uintptr_t substitute_hkl = 0;
    uintptr_t hkl = 0;
    std::wstring api_description;
    ProfileMetadata registry;
    BOOL enabled = FALSE;
    HRESULT active_result = E_UNEXPECTED;
    LANGID active_language = 0;
    GUID active_profile = GUID_NULL;
    HRESULT default_result = E_UNEXPECTED;
    CLSID default_service = CLSID_NULL;
    GUID default_profile = GUID_NULL;
    std::vector<GUID> categories;
};

class ProfileDisplayBackend {
public:
    virtual ~ProfileDisplayBackend() = default;
    virtual HRESULT read(ProfileSnapshot* snapshot) = 0;
    // The adapter must pass the captured icon path/index and all profile
    // metadata required by RegisterProfile; only the description may change.
    virtual HRESULT write_description(const ProfileSnapshot& captured,
                                      const std::wstring& description) = 0;
};

struct UpdateResult {
    HRESULT operation = E_NOTIMPL;
    HRESULT cleanup = S_OK;
    bool rollback_attempted = false;
    HRESULT rollback = S_FALSE;
    bool rollback_verified = false;
};

namespace detail {
inline bool guid_equal(const GUID& a,const GUID& b) { return IsEqualGUID(a,b)!=FALSE; }
inline bool same_except_descriptions(const ProfileSnapshot& a,const ProfileSnapshot& b) {
    if(a.profile_type!=b.profile_type||a.language!=b.language||
       !guid_equal(a.service,b.service)||!guid_equal(a.profile,b.profile)||
       !guid_equal(a.category,b.category)||a.capabilities!=b.capabilities||a.flags!=b.flags||
       a.substitute_hkl!=b.substitute_hkl||a.hkl!=b.hkl||
       a.registry.icon_file!=b.registry.icon_file||a.registry.icon_index!=b.registry.icon_index||
       a.registry.enabled_by_default!=b.registry.enabled_by_default||
       a.registry.hidden_in_settings!=b.registry.hidden_in_settings||
       a.registry.subitem_in_settings!=b.registry.subitem_in_settings||
       a.enabled!=b.enabled||a.active_result!=b.active_result||
       a.active_language!=b.active_language||!guid_equal(a.active_profile,b.active_profile)||
       a.default_result!=b.default_result||!guid_equal(a.default_service,b.default_service)||
       !guid_equal(a.default_profile,b.default_profile)||a.categories.size()!=b.categories.size())return false;
    for(size_t i=0;i<a.categories.size();++i)if(!guid_equal(a.categories[i],b.categories[i]))return false;
    return true;
}
inline bool same_snapshot(const ProfileSnapshot& a,const ProfileSnapshot& b) {
    return a.api_description==b.api_description&&
           a.registry.description==b.registry.description&&same_except_descriptions(a,b);
}
inline bool usable_expected(const ProfileSnapshot& s) {
    return s.profile_type!=0&&s.language!=0&&!guid_equal(s.service,CLSID_NULL)&&
           !guid_equal(s.profile,GUID_NULL)&&!guid_equal(s.category,GUID_NULL)&&
           !s.registry.icon_file.empty()&&s.api_description==s.registry.description;
}
inline bool only_our_description_write(const ProfileSnapshot& before,const ProfileSnapshot& after,
                                       const std::wstring& target) {
    if(!same_except_descriptions(before,after))return false;
    const bool api_known=after.api_description==before.api_description||after.api_description==target;
    const bool registry_known=after.registry.description==before.registry.description||
                              after.registry.description==target;
    return api_known&&registry_known;
}
}

inline UpdateResult update_description(ProfileDisplayBackend& backend,const ProfileSnapshot& expected,
                                       const std::wstring& description=kDisplayDescription) {
    if(description!=kDisplayDescription||!detail::usable_expected(expected))
        return {E_INVALIDARG,S_OK,false,S_FALSE,false};
    ProfileSnapshot before{};
    HRESULT status=backend.read(&before);
    if(FAILED(status))return {status,S_OK,false,S_FALSE,false};
    if(!detail::same_snapshot(before,expected))
        return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),S_OK,false,S_FALSE,false};
    if(before.api_description==description&&before.registry.description==description)
        return {S_FALSE,S_OK,false,S_FALSE,false};

    const HRESULT write_status=backend.write_description(before,description);
    ProfileSnapshot after{};
    const HRESULT read_status=backend.read(&after);
    if(FAILED(read_status))
        return {FAILED(write_status)?write_status:read_status,read_status,false,S_FALSE,false};

    const bool desired_state=detail::same_except_descriptions(before,after)&&
        after.api_description==description&&after.registry.description==description;
    if(SUCCEEDED(write_status)&&desired_state)return {S_OK,S_OK,false,S_FALSE,false};

    const HRESULT operation=FAILED(write_status)?write_status:
        HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if(detail::same_snapshot(before,after))return {operation,S_OK,false,S_FALSE,false};
    if(!detail::only_our_description_write(before,after,description))
        return {operation,HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),false,S_FALSE,false};

    const HRESULT rollback=backend.write_description(after,before.api_description);
    ProfileSnapshot restored{};
    const HRESULT restore_read=backend.read(&restored);
    if(FAILED(restore_read))
        return {operation,restore_read,true,rollback,false};
    const bool verified=detail::same_snapshot(before,restored);
    const HRESULT cleanup=verified?S_OK:
        (FAILED(rollback)?rollback:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    return {operation,cleanup,true,rollback,verified};
}

} // namespace boshiamy::profile_display
