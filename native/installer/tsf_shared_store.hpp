#ifndef LIUKAI_INSTALLER_TSF_SHARED_STORE_HPP
#define LIUKAI_INSTALLER_TSF_SHARED_STORE_HPP

#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "windows_backend.hpp"
#include "../tsf/windows_profile_display_update.hpp"
#include "../tsf/profile_icon_relocation.hpp"
#include "profile_uninstall_cleanup.hpp"
#include "independent_categories.hpp"

namespace boshiamy::installer {

inline const CLSID kInstallerTsfClsid = {0x5d7c4291, 0x2b34, 0x4ce8, {0x8a, 0x11, 0x39, 0x62, 0x71, 0x05, 0xc4, 0x2e}};
inline const GUID kInstallerProfileGuid = {0x7254a21f, 0x6eb1, 0x4217, {0xa4, 0x2c, 0xc2, 0x73, 0x01, 0x17, 0x5d, 0x90}};
inline const GUID kInstallerTrayCategory = {0x25504fb4, 0x7bab, 0x4bc1, {0x9c, 0x69, 0xcf, 0x81, 0x89, 0x0f, 0x0e, 0xf5}};
inline const GUID kInstallerUiElementCategory = {0x49d2f9cf, 0x1f5e, 0x11d7, {0xa6, 0xd3, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};
inline constexpr LANGID kInstallerProfileLanguage = 0x0404;
// TF_IPP_CAPS_SYSTRAYSUPPORT is omitted from the bundled MinGW msctf.h.
inline constexpr DWORD kInstallerProfileCapsSysTraySupport = 0x00020000;
inline constexpr DWORD kInstallerProfileCapsUiElementEnabled = 0x00000004;
inline constexpr uint32_t kProfileUserStateMagic = 0x3155534c; // LSU1

class TsfSharedApi {
public:
    virtual ~TsfSharedApi() = default;
    virtual HRESULT profile_present(bool* present) = 0;
    virtual HRESULT profile_presence_after_write(bool, bool* present) { return profile_present(present); }
    virtual HRESULT profile_capabilities(DWORD* capabilities) {
        if (!capabilities) return E_POINTER;
        *capabilities = 0;
        return E_NOTIMPL;
    }
    virtual HRESULT profile_capabilities_with_required(DWORD, DWORD* capabilities) {
        return profile_capabilities(capabilities);
    }
    // Optional metadata support. Legacy fake/host APIs retain their old path
    // policy when no independently read icon metadata is available.
    virtual HRESULT profile_icon_path(std::wstring* path) {
        if (!path) return E_POINTER;
        path->clear(); return E_NOTIMPL;
    }
    virtual HRESULT relocate_profile_icon(const std::wstring&, const std::wstring&) { return E_NOTIMPL; }
    virtual HRESULT register_profile(const std::wstring& icon_path, bool enabled_by_default) = 0;
    virtual HRESULT unregister_profile() = 0;
    virtual HRESULT profile_user_state(bool* enabled, bool* is_default) = 0;
    virtual HRESULT restore_profile_user_state(bool enabled, bool is_default) = 0;
    virtual HRESULT category_present(Resource resource, bool* present) = 0;
    virtual HRESULT category_presence_after_write(Resource resource, bool, bool* present) {
        return category_present(resource, present);
    }
    virtual HRESULT register_category(Resource resource) = 0;
    virtual HRESULT unregister_category(Resource resource) = 0;
};

class WindowsTsfSharedApi final : public TsfSharedApi {
    template<class T> class ComPtr {
        T* value_ = nullptr;
    public:
        ~ComPtr() { if (value_) value_->Release(); }
        T* get() const { return value_; }
        T** put() { if (value_) { value_->Release(); value_ = nullptr; } return &value_; }
        ComPtr() = default;
        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;
    };
    ComPtr<ITfInputProcessorProfileMgr> profiles_;
    ComPtr<ITfInputProcessorProfiles> user_profiles_;
    ComPtr<ITfCategoryMgr> categories_;
    HRESULT ready_ = E_UNEXPECTED;
    bool profile_relocation_uncertain_ = false;
    bool profile_was_unregistered_ = false;

    HRESULT profile_manager_status(bool* present) {
        if (!present) return E_POINTER;
        *present = false;
        if (FAILED(ready_)) return ready_;
        ComPtr<IEnumTfInputProcessorProfiles> enumeration;
        HRESULT hr = profiles_.get()->EnumProfiles(kInstallerProfileLanguage, enumeration.put());
        if (hr != S_OK) return hr;
        if (!enumeration.get()) return E_UNEXPECTED;
        for (;;) {
            TF_INPUTPROCESSORPROFILE row{}; ULONG count = 0;
            hr = enumeration.get()->Next(1, &row, &count);
            if (hr == S_FALSE && !count) return S_OK;
            if (hr != S_OK) return hr;
            if (count != 1) return E_UNEXPECTED;
            if (row.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR && row.langid == kInstallerProfileLanguage &&
                IsEqualCLSID(row.clsid, kInstallerTsfClsid) && IsEqualGUID(row.guidProfile, kInstallerProfileGuid)) {
                *present = true; return S_OK;
            }
        }
    }
    static const GUID* category_id(Resource resource) {
        if (resource == Resource::keyboard_category) return &GUID_TFCAT_TIP_KEYBOARD;
        if (resource == Resource::system_tray_category) return &kInstallerTrayCategory;
        if (resource == Resource::ui_element_category) return &kInstallerUiElementCategory;
        return nullptr;
    }
public:
    WindowsTsfSharedApi() {
        ready_ = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(profiles_.put()));
        if (SUCCEEDED(ready_)) ready_ = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(user_profiles_.put()));
        if (SUCCEEDED(ready_)) ready_ = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfCategoryMgr, reinterpret_cast<void**>(categories_.put()));
    }
    HRESULT ready() const { return ready_; }
    HRESULT profile_present(bool* present) override {
        if (profile_relocation_uncertain_) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if (FAILED(ready_)) return ready_;
        if (profile_was_unregistered_) return independent_categories::read_profile_present_fresh(present);
        return profile_manager_status(present);
    }
    HRESULT profile_presence_after_write(bool expected, bool* present) override {
        return independent_categories::retry_presence(
            [this](bool* value) { return profile_present(value); }, [] { Sleep(250); },
            expected, 40, present);
    }
    HRESULT profile_icon_path(std::wstring* path) override {
        if (!path) return E_POINTER;
        path->clear();
        if (profile_relocation_uncertain_) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return profile_display::windows_detail::read_profile_icon_path(
            HKEY_LOCAL_MACHINE, profile_display::windows_detail::profile_key_path(), path);
    }
    HRESULT relocate_profile_icon(const std::wstring& expected, const std::wstring& target) override {
        if (profile_relocation_uncertain_) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        class Access final : public profile_icon::Access {
            profile_display::WindowsProfileDisplayBackend profiles_;
        public:
            HRESULT read(profile_icon::Snapshot* out) override {
                return profiles_.ready() == S_OK ? profiles_.read(out) : profiles_.ready();
            }
            HRESULT write(const profile_icon::Snapshot& before, const std::wstring& icon) override {
                return profiles_.write_icon_path(before, icon);
            }
        } access;
        const auto result = profile_icon::relocate(access, expected, target);
        profile_relocation_uncertain_ = result.uncertain || result.cleanup != S_OK;
        return result.operation;
    }
    HRESULT profile_capabilities(DWORD* capabilities) override {
        return profile_capabilities_with_required(0,capabilities);
    }
    HRESULT profile_capabilities_with_required(DWORD required, DWORD* capabilities) override {
        if (!capabilities) return E_POINTER;
        *capabilities = 0;
        if (FAILED(ready_)) return ready_;
        bool present=false;
        const HRESULT observed=profile_manager_status(&present);
        if(observed!=S_OK)return observed;
        return independent_categories::read_profile_caps_fresh(capabilities,present,required);
    }
    HRESULT register_profile(const std::wstring& icon_path, bool enabled_by_default) override {
        if (FAILED(ready_)) return ready_;
        const std::wstring description = L"嘸．改";
        HRESULT status = profiles_.get()->RegisterProfile(kInstallerTsfClsid, kInstallerProfileLanguage, kInstallerProfileGuid,
            description.c_str(), static_cast<ULONG>(description.size()), icon_path.c_str(), static_cast<ULONG>(icon_path.size()),
            0, nullptr, 0, enabled_by_default ? TRUE : FALSE, 0);
        if (status == S_OK) profile_was_unregistered_ = false;
        return status;
    }
    HRESULT unregister_profile() override {
        if (FAILED(ready_)) return ready_;
        profile_uninstall::WindowsStore residue(profile_display::windows_detail::profile_key_path());
        profile_uninstall::UserState before;
        HRESULT status = residue.read_user(&before);
        if (status != S_OK) return status;
        status = profiles_.get()->UnregisterProfile(kInstallerTsfClsid, kInstallerProfileLanguage, kInstallerProfileGuid, 0);
        if (status != S_OK) return status;
        profile_was_unregistered_ = true;
        return profile_uninstall::finish(residue, before);
    }
    HRESULT profile_user_state(bool* enabled, bool* is_default) override {
        if (!enabled || !is_default) return E_POINTER;
        *enabled = false; *is_default = false;
        if (FAILED(ready_)) return ready_;
        BOOL enabled_value = FALSE;
        HRESULT hr = user_profiles_.get()->IsEnabledLanguageProfile(kInstallerTsfClsid, kInstallerProfileLanguage,
            kInstallerProfileGuid, &enabled_value);
        if (hr != S_OK) return hr;
        *enabled = enabled_value != FALSE;
        CLSID default_clsid{}; GUID default_profile{};
        hr = user_profiles_.get()->GetDefaultLanguageProfile(kInstallerProfileLanguage, GUID_TFCAT_TIP_KEYBOARD,
            &default_clsid, &default_profile);
        if (hr == S_FALSE) return S_OK;
        if (hr != S_OK) return hr;
        *is_default = IsEqualCLSID(default_clsid, kInstallerTsfClsid) && IsEqualGUID(default_profile, kInstallerProfileGuid);
        return S_OK;
    }
    HRESULT restore_profile_user_state(bool enabled, bool is_default) override {
        if (FAILED(ready_)) return ready_;
        HRESULT hr = user_profiles_.get()->EnableLanguageProfile(kInstallerTsfClsid, kInstallerProfileLanguage,
            kInstallerProfileGuid, enabled ? TRUE : FALSE);
        if (FAILED(hr)) return hr;
        if (is_default) return user_profiles_.get()->SetDefaultLanguageProfile(kInstallerProfileLanguage,
            kInstallerTsfClsid, kInstallerProfileGuid);
        return S_OK;
    }
    HRESULT category_present(Resource resource, bool* present) override {
        if (!present) return E_POINTER;
        *present = false;
        if (FAILED(ready_)) return ready_;
        const GUID* category = category_id(resource);
        if (!category) return E_INVALIDARG;
        std::vector<GUID> categories;
        const HRESULT hr = independent_categories::read_fresh(&categories);
        if (hr != S_OK) return hr;
        *present = std::any_of(categories.begin(),categories.end(),[&](const GUID& item){return IsEqualGUID(item,*category);});
        return S_OK;
    }
    HRESULT category_presence_after_write(Resource resource, bool expected, bool* present) override {
        return independent_categories::retry_presence(
            [this,resource](bool* value) { return category_present(resource, value); }, [] { Sleep(250); },
            expected, 40, present);
    }
    HRESULT register_category(Resource resource) override {
        if (FAILED(ready_)) return ready_;
        const GUID* category = category_id(resource);
        return category ? categories_.get()->RegisterCategory(kInstallerTsfClsid, *category, kInstallerTsfClsid) : E_INVALIDARG;
    }
    HRESULT unregister_category(Resource resource) override {
        if (FAILED(ready_)) return ready_;
        const GUID* category = category_id(resource);
        return category ? categories_.get()->UnregisterCategory(kInstallerTsfClsid, *category, kInstallerTsfClsid) : E_INVALIDARG;
    }
};

struct OwnerMarker {
    bool present = false;
    std::wstring owner;
    std::wstring path;
    bool enabled_by_default = true;
    std::vector<unsigned char> encoded;
};

inline void encode_owner_marker(const OwnerMarker& marker, std::vector<unsigned char>* out) {
    out->clear(); const uint32_t version = 1, owner_chars = static_cast<uint32_t>(marker.owner.size()), path_chars = static_cast<uint32_t>(marker.path.size());
    auto add = [&](const void* value, size_t count) { const auto* p = static_cast<const unsigned char*>(value); out->insert(out->end(), p, p + count); };
    add(&version, sizeof(version)); add(&owner_chars, sizeof(owner_chars)); add(marker.owner.data(), marker.owner.size() * sizeof(wchar_t));
    add(&path_chars, sizeof(path_chars)); add(marker.path.data(), marker.path.size() * sizeof(wchar_t));
    const uint32_t enabled = marker.enabled_by_default ? 1 : 0; add(&enabled, sizeof(enabled));
}

class OwnerMarkerStore {
public:
    virtual ~OwnerMarkerStore() = default;
    virtual HRESULT read(Resource resource, OwnerMarker* marker) = 0;
    virtual HRESULT write(Resource resource, const OwnerMarker& marker) = 0;
    virtual HRESULT remove(Resource resource) = 0;
};

enum class SharedStoreTracePhase : uint8_t { Read, Apply, ApplyCompensation, Restore, RestoreCompensation, Health };
enum class SharedStoreTraceStage : uint8_t {
    StateRevalidation, CategoryReadback, MarkerRead, RegisterCategory, CategoryReadbackAfterRegister,
    ProfileCapabilities, UnregisterCategory, UnregisterCategoryCompensation, CategoryReadbackAfterCompensation,
    MarkerWrite, MarkerRemove, FinalReadback, FinalVerification,
    ProfileReadback, RegisterProfile, UnregisterProfile, ProfileIconReadback,
    ProfileUserStateRestore, ProfileUserStateReadback, ProfileUserStateVerification,
    FinalUserStateVerification, FinalMarkerVerification, PostApplyVerification
};
struct SharedStoreTraceEvent {
    SharedStoreTracePhase phase = SharedStoreTracePhase::Read;
    SharedStoreTraceStage stage = SharedStoreTraceStage::StateRevalidation;
    Resource resource = Resource::count;
    HRESULT status = S_OK;
    bool present_known = false;
    bool present = false;
    bool capabilities_known = false;
    DWORD capabilities = 0;
    DWORD required_capability = 0;
    bool user_state_known = false;
    bool enabled = false;
    bool is_default = false;
};
class SharedStoreTrace {
    static constexpr size_t kCapacity = 128;
    std::array<SharedStoreTraceEvent, kCapacity> events_{};
    size_t count_ = 0;
    size_t dropped_ = 0;
public:
    void record(const SharedStoreTraceEvent& event) noexcept {
        if (count_ < kCapacity) {
            events_[count_++] = event;
            return;
        }
        // Read-heavy deploys can produce more successful observations than the
        // fixed buffer holds before a useful failure is reached. Keep failures
        // by removing the oldest successful observation and appending the
        // failure, so retained events remain in chronological order. Never
        // allocate or alter the operation being diagnosed.
        if (FAILED(event.status)) {
            for (size_t i = 0; i < count_; ++i) {
                if (SUCCEEDED(events_[i].status)) {
                    for (size_t j = i + 1; j < count_; ++j) events_[j - 1] = events_[j];
                    events_[count_ - 1] = event;
                    ++dropped_;
                    return;
                }
            }
        }
        ++dropped_;
    }
    const SharedStoreTraceEvent* data() const noexcept { return events_.data(); }
    size_t size() const noexcept { return count_; }
    size_t dropped_count() const noexcept { return dropped_; }
};

class RegistryOwnerMarkerStore final : public OwnerMarkerStore {
    HKEY hive_;
    std::wstring root_;
    std::wstring service_key_;
    std::wstring owner_;
    static const wchar_t* resource_key(Resource resource) {
        if (resource == Resource::profile) return L"Profile";
        if (resource == Resource::keyboard_category) return L"KeyboardCategory";
        if (resource == Resource::system_tray_category) return L"SystemTrayCategory";
        if (resource == Resource::ui_element_category) return L"UiElementCategory";
        return nullptr;
    }
    std::wstring key_path(Resource resource) const {
        return root_ + L"\\" + service_key_ + L"\\" + resource_key(resource);
    }
    static HRESULT win32(LONG status) { return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }
    static bool decode(const std::vector<unsigned char>& bytes, OwnerMarker* marker) {
        size_t offset = 0;
        auto read_u32 = [&](uint32_t* out) {
            if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t)) return false;
            std::memcpy(out, bytes.data() + offset, sizeof(uint32_t)); offset += sizeof(uint32_t); return true;
        };
        uint32_t version = 0, owner_chars = 0, path_chars = 0, enabled = 0;
        if (!read_u32(&version) || version != 1 || !read_u32(&owner_chars) || owner_chars > 32760 ||
            size_t(owner_chars) > (bytes.size() - offset) / sizeof(wchar_t)) return false;
        marker->owner.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), owner_chars); offset += size_t(owner_chars) * sizeof(wchar_t);
        if (!read_u32(&path_chars) || path_chars > 32760 || size_t(path_chars) > (bytes.size() - offset) / sizeof(wchar_t)) return false;
        marker->path.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), path_chars); offset += size_t(path_chars) * sizeof(wchar_t);
        if (!read_u32(&enabled) || enabled > 1 || offset != bytes.size()) return false;
        marker->present = true; marker->enabled_by_default = enabled != 0; marker->encoded = bytes;
        return true;
    }
public:
    RegistryOwnerMarkerStore(HKEY hive, std::wstring root, REFCLSID service, std::wstring owner)
        : hive_(hive), root_(std::move(root)), owner_(std::move(owner)) {
        wchar_t id[40]{}; if (StringFromGUID2(service, id, 40) > 0) service_key_ = id;
        if (!service_key_.empty() && service_key_.back() == L'\0') service_key_.pop_back();
    }
    HRESULT read(Resource resource, OwnerMarker* marker) override {
        if (!marker) return E_POINTER;
        const wchar_t* leaf = resource_key(resource); if (!leaf || service_key_.empty()) return E_INVALIDARG;
        *marker = OwnerMarker{};
        HKEY key = nullptr; LONG status = RegOpenKeyExW(hive_, key_path(resource).c_str(), 0, KEY_READ, &key);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return S_OK;
        if (status != ERROR_SUCCESS) return win32(status);
        DWORD type = 0, size = 0;
        status = RegQueryValueExW(key, L"Record", nullptr, &type, nullptr, &size);
        std::vector<unsigned char> bytes;
        HRESULT hr = status == ERROR_SUCCESS && type == REG_BINARY && size <= 256 * 1024 ? S_OK :
            status == ERROR_SUCCESS ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : win32(status);
        if (SUCCEEDED(hr)) {
            bytes.resize(size);
            status = RegQueryValueExW(key, L"Record", nullptr, &type, bytes.empty() ? nullptr : bytes.data(), &size);
            if (status != ERROR_SUCCESS) hr = win32(status);
            else if (!decode(bytes, marker)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        RegCloseKey(key);
        return hr;
    }
    HRESULT write(Resource resource, const OwnerMarker& marker) override {
        if (marker.owner != owner_ || !marker.present || !resource_key(resource)) return E_INVALIDARG;
        HKEY key = nullptr; DWORD disposition = 0;
        LONG status = RegCreateKeyExW(hive_, key_path(resource).c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
            nullptr, &key, &disposition);
        if (status != ERROR_SUCCESS) return win32(status);
        std::vector<unsigned char> bytes; encode_owner_marker(marker, &bytes);
        HRESULT hr = win32(RegSetValueExW(key, L"Record", 0, REG_BINARY,
            bytes.empty() ? nullptr : bytes.data(), static_cast<DWORD>(bytes.size())));
        RegCloseKey(key);
        if (FAILED(hr)) return hr;
        OwnerMarker verify; hr = read(resource, &verify);
        if (FAILED(hr)) return hr;
        return verify.present && verify.encoded == bytes ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    HRESULT remove(Resource resource) override {
        if (!resource_key(resource)) return E_INVALIDARG;
        const std::wstring path = key_path(resource);
        LONG status = RegDeleteTreeW(hive_, path.c_str());
        return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ? S_OK : win32(status);
    }
};

// Borrowed metadata relocation is opt-in at the validated package-deployment
// composition root. It changes no lifecycle ownership and never adopts markers.
enum class ProfileRelocationPolicy { owned_only, package_borrowed_metadata };

class WindowsTsfSharedStore final : public SharedResourceStore {
    TsfSharedApi& api_;
    OwnerMarkerStore& markers_;
    std::wstring owner_;
    SharedStoreTrace* trace_ = nullptr;
    ProfileRelocationPolicy relocation_policy_;
    ResourceState applied_target_[4]{};
    ResourceState applied_state_[4]{};
    bool applied_valid_[4]{};
    std::array<ResourceState, 4> last_{};
    std::array<bool, 4> valid_{};
    std::array<bool, 4> pending_created_{};
    std::array<bool, 4> uncertain_creation_{};
    std::array<std::wstring, 4> pending_path_{};
    static size_t slot(Resource resource) {
        return resource == Resource::profile ? 0 : resource == Resource::keyboard_category ? 1 :
            resource == Resource::system_tray_category ? 2 : 3;
    }
    static bool is_shared(Resource resource) {
        return resource == Resource::profile || resource == Resource::keyboard_category ||
            resource == Resource::system_tray_category || resource == Resource::ui_element_category;
    }
    void trace(SharedStoreTracePhase phase, SharedStoreTraceStage stage, Resource resource,
               HRESULT status, bool present_known = false, bool present = false,
               bool capabilities_known = false, DWORD capabilities = 0, DWORD required = 0) const noexcept {
        if (!trace_ || !is_shared(resource)) return;
        trace_->record({phase, stage, resource, status, present_known, present,
                        capabilities_known, capabilities, required});
    }
    void trace_user(SharedStoreTracePhase phase, SharedStoreTraceStage stage,
                    HRESULT status, bool known, bool enabled, bool is_default) const noexcept {
        if (!trace_) return;
        SharedStoreTraceEvent event{phase, stage, Resource::profile, status};
        event.user_state_known = known;
        event.enabled = enabled;
        event.is_default = is_default;
        trace_->record(event);
    }
    HRESULT read_uncached(Resource resource, ResourceState* state) {
        if (!state) return E_POINTER;
        if (!is_shared(resource)) return E_INVALIDARG;
        *state = ResourceState{};
        bool present = false; HRESULT hr = resource == Resource::profile ? api_.profile_present(&present) : api_.category_present(resource, &present);
        trace(SharedStoreTracePhase::Read, resource == Resource::profile ? SharedStoreTraceStage::ProfileReadback :
              SharedStoreTraceStage::CategoryReadback, resource, hr, hr == S_OK, present);
        if (hr != S_OK) return hr;
        // A failed registration API is allowed to have side effects. Without
        // durable ownership proof, neither presence nor absence can be adopted
        // as a completed mutation or used to remove another actor's relation.
        if (uncertain_creation_[slot(resource)])
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        // After a successful create, an absent TSF read may be a delayed
        // observation. It is not evidence that compensation has completed.
        // Keep the operation pending and fail closed until it can be observed
        // or an explicit removal has independently established absence.
        if (!present && pending_created_[slot(resource)])
            return HRESULT_FROM_WIN32(ERROR_RETRY);
        OwnerMarker marker; hr = markers_.read(resource, &marker);
        trace(SharedStoreTracePhase::Read, SharedStoreTraceStage::MarkerRead, resource, hr);
        if (hr != S_OK) return hr;
        state->present = present;
        state->owner = marker.present ? marker.owner : (present ? owner_ : L"");
        const size_t index = slot(resource);
        state->created_by_lifecycle = (marker.present && marker.owner == owner_) || pending_created_[index];
        state->path = marker.present && marker.owner == owner_ ? marker.path : (pending_created_[index] ? pending_path_[index] : L"");
        state->private_data = marker.encoded;
        if (resource == Resource::profile && present) {
            std::wstring actual_icon;
            const HRESULT icon_status = api_.profile_icon_path(&actual_icon);
            trace(SharedStoreTracePhase::Read, SharedStoreTraceStage::ProfileIconReadback, resource, icon_status);
            if (icon_status == S_OK) {
                if (actual_icon.empty()) {
                    trace(SharedStoreTracePhase::Read, SharedStoreTraceStage::ProfileIconReadback,
                          resource, HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
                if (pending_created_[index] && !marker.present && actual_icon != pending_path_[index])
                    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                if (marker.present && marker.owner == owner_ && actual_icon != marker.path &&
                    !(pending_created_[index] && actual_icon == pending_path_[index]))
                    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                state->path = actual_icon;
            } else if (icon_status != E_NOTIMPL) return icon_status;
            bool enabled = false, is_default = false;
            hr = api_.profile_user_state(&enabled, &is_default);
            trace_user(SharedStoreTracePhase::Read, SharedStoreTraceStage::ProfileUserStateReadback,
                       hr, hr == S_OK, enabled, is_default);
            if (hr != S_OK) return hr;
            const uint32_t magic = kProfileUserStateMagic;
            const auto* tag = reinterpret_cast<const unsigned char*>(&magic);
            state->private_data.insert(state->private_data.end(), tag, tag + sizeof(magic));
            state->private_data.push_back(enabled ? 1 : 0);
            state->private_data.push_back(is_default ? 1 : 0);
        }
        return S_OK;
    }
    static bool decode_marker_prefix(const std::vector<unsigned char>& bytes, OwnerMarker* marker) {
        if (!marker) return false;
        size_t offset = 0;
        auto read_u32 = [&](uint32_t* out) {
            if (bytes.size() - offset < sizeof(uint32_t)) return false;
            std::memcpy(out, bytes.data() + offset, sizeof(uint32_t)); offset += sizeof(uint32_t); return true;
        };
        uint32_t version = 0, owner_chars = 0, path_chars = 0, enabled = 0;
        if (!read_u32(&version) || version != 1 || !read_u32(&owner_chars) || owner_chars > 32760 ||
            size_t(owner_chars) > (bytes.size() - offset) / sizeof(wchar_t)) return false;
        marker->owner.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), owner_chars); offset += size_t(owner_chars) * sizeof(wchar_t);
        if (!read_u32(&path_chars) || path_chars > 32760 || size_t(path_chars) > (bytes.size() - offset) / sizeof(wchar_t)) return false;
        marker->path.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), path_chars); offset += size_t(path_chars) * sizeof(wchar_t);
        if (!read_u32(&enabled) || enabled > 1) return false;
        marker->present = true; marker->enabled_by_default = enabled != 0;
        marker->encoded.assign(bytes.begin(), bytes.begin() + offset);
        return true;
    }
    static bool decode_profile_user_state(const std::vector<unsigned char>& bytes, bool* enabled, bool* is_default) {
        if (!enabled || !is_default) return false;
        OwnerMarker marker;
        if (!decode_marker_prefix(bytes, &marker) || marker.encoded.size() > bytes.size() ||
            bytes.size() - marker.encoded.size() != sizeof(uint32_t) + 2) return false;
        uint32_t magic = 0;
        std::memcpy(&magic, bytes.data() + marker.encoded.size(), sizeof(magic));
        const size_t tail = marker.encoded.size() + sizeof(magic);
        const unsigned char enabled_value = bytes[tail];
        const unsigned char default_value = bytes[tail + 1];
        if (magic != kProfileUserStateMagic || enabled_value > 1 || default_value > 1) return false;
        *enabled = enabled_value != 0;
        *is_default = default_value != 0;
        return true;
    }
    HRESULT make_marker(Resource resource, const ResourceState& desired, OwnerMarker* marker) {
        if (!marker) return E_POINTER;
        *marker = OwnerMarker{}; marker->present = desired.created_by_lifecycle;
        marker->owner = desired.owner; marker->path = resource == Resource::profile ? desired.path : L"";
        marker->enabled_by_default = true;
        OwnerMarker prior;
        if (decode_marker_prefix(desired.private_data, &prior) && prior.owner == owner_)
            marker->enabled_by_default = prior.enabled_by_default;
        encode_owner_marker(*marker, &marker->encoded);
        return S_OK;
    }
    struct ProfileUserState {
        bool enabled = false;
        bool is_default = false;
    };
    // Only for states produced by read_uncached, including borrowed profiles
    // whose LSU1 suffix has no owner-marker prefix.
    static bool observed_profile_user_state(const ResourceState& state, ProfileUserState* user) {
        const size_t suffix_size = sizeof(uint32_t) + 2;
        if (!user || !state.present || state.private_data.size() < suffix_size) return false;
        const size_t offset = state.private_data.size() - suffix_size;
        uint32_t magic = 0;
        std::memcpy(&magic, state.private_data.data() + offset, sizeof(magic));
        const unsigned char enabled = state.private_data[offset + sizeof(magic)];
        const unsigned char is_default = state.private_data[offset + sizeof(magic) + 1];
        if (magic != kProfileUserStateMagic || enabled > 1 || is_default > 1) return false;
        user->enabled = enabled != 0;
        user->is_default = is_default != 0;
        return true;
    }
    HRESULT set_api_presence(Resource resource, bool present, const ResourceState& target,
                             SharedStoreTracePhase phase, ProfileUserState* registered_user = nullptr,
                             bool compensation = false) {
        if (resource == Resource::profile) {
            if (present) {
                OwnerMarker marker; HRESULT hr = make_marker(resource, target, &marker); if (FAILED(hr)) return hr;
                hr = api_.register_profile(target.path, false);
                trace(phase, SharedStoreTraceStage::RegisterProfile, resource, hr);
                if (FAILED(hr)) {
                    uncertain_creation_[slot(resource)] = true;
                    return hr;
                }
                pending_created_[slot(resource)] = true; pending_path_[slot(resource)] = target.path;
                bool enabled = true, is_default = false;
                // Only an exact tagged suffix after a valid marker is a snapshot.
                // Empty, new, and orphan-marker states are fresh installs.
                decode_profile_user_state(target.private_data, &enabled, &is_default);
                hr = api_.restore_profile_user_state(enabled, is_default);
                // This stage records the requested state. The next stage records
                // the independently observed state, without additional API calls.
                trace_user(phase, SharedStoreTraceStage::ProfileUserStateRestore, hr, true, enabled, is_default);
                if (FAILED(hr)) return hr;
                bool observed_enabled = false, observed_default = false;
                hr = api_.profile_user_state(&observed_enabled, &observed_default);
                trace_user(phase, SharedStoreTraceStage::ProfileUserStateReadback,
                           hr, hr == S_OK, observed_enabled, observed_default);
                if (hr != S_OK) return hr;
                if (observed_enabled != enabled || (is_default && !observed_default)) {
                    trace_user(phase, SharedStoreTraceStage::ProfileUserStateVerification,
                               HRESULT_FROM_WIN32(ERROR_INVALID_DATA), true, observed_enabled, observed_default);
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                }
                // A false default request does not clear an existing default.
                // Preserve the actual verified value for the later readback.
                if (registered_user) *registered_user = {observed_enabled, observed_default};
                return S_OK;
            }
            const HRESULT status = api_.unregister_profile();
            trace(phase, SharedStoreTraceStage::UnregisterProfile, resource, status);
            return status;
        }
        if (present) {
            HRESULT hr = api_.register_category(resource);
            trace(phase, SharedStoreTraceStage::RegisterCategory, resource, hr);
            if (hr == S_OK) pending_created_[slot(resource)] = true;
            else if (FAILED(hr)) uncertain_creation_[slot(resource)] = true;
            else return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            return hr == S_OK ? S_OK : FAILED(hr) ? hr : E_FAIL;
        }
        const HRESULT status = api_.unregister_category(resource);
        trace(compensation ? (phase == SharedStoreTracePhase::Apply ? SharedStoreTracePhase::ApplyCompensation :
                              SharedStoreTracePhase::RestoreCompensation) : phase,
              compensation ? SharedStoreTraceStage::UnregisterCategoryCompensation : SharedStoreTraceStage::UnregisterCategory,
              resource, status);
        return status;
    }
    HRESULT write_marker_for(Resource resource, const ResourceState& desired, SharedStoreTracePhase phase) {
        if (!desired.present || !desired.created_by_lifecycle) {
            const HRESULT status = markers_.remove(resource);
            trace(phase, SharedStoreTraceStage::MarkerRemove, resource, status);
            return status;
        }
        OwnerMarker marker; HRESULT hr = make_marker(resource, desired, &marker);
        if (FAILED(hr)) { trace(phase, SharedStoreTraceStage::MarkerWrite, resource, hr); return hr; }
        hr = markers_.write(resource, marker);
        trace(phase, SharedStoreTraceStage::MarkerWrite, resource, hr);
        return hr;
    }
    HRESULT transition(Resource resource, const ResourceState& current, const ResourceState& target,
                       SharedStoreTracePhase phase, ResourceState* verified_write = nullptr) {
        ResourceState icon_only_target = target;
        icon_only_target.path = current.path;
        const bool borrowed_icon_only = relocation_policy_ == ProfileRelocationPolicy::package_borrowed_metadata &&
            resource == Resource::profile && current.present && !current.created_by_lifecycle &&
            target.present && !target.created_by_lifecycle && current.owner == owner_ &&
            target.owner == owner_ && icon_only_target == current;
        if (current.present && !current.created_by_lifecycle && (!target.present || target != current) &&
            !borrowed_icon_only) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        OwnerMarker marker; HRESULT hr = markers_.read(resource, &marker);
        trace(phase, SharedStoreTraceStage::MarkerRead, resource, hr);
        if (hr != S_OK) return hr;
        if (marker.present && marker.owner != owner_) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        const bool target_present = target.present;
        ProfileUserState expected_user;
        if (resource == Resource::profile && target_present && current.present &&
            !observed_profile_user_state(current, &expected_user))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        // Build the exact marker before writing. A first readback with the same
        // owner/path but changed opaque fields must not become our baseline.
        std::vector<unsigned char> expected_marker;
        if (target_present && target.created_by_lifecycle) {
            OwnerMarker desired_marker;
            hr = make_marker(resource, target, &desired_marker); if (hr != S_OK) return hr;
            expected_marker = std::move(desired_marker.encoded);
        }
        const bool relation_was_present = current.present;
        if (resource == Resource::profile && current.present && target_present &&
            (current.created_by_lifecycle || borrowed_icon_only) && current.path != target.path) {
            std::wstring actual_icon;
            hr = api_.profile_icon_path(&actual_icon);
            trace(phase, SharedStoreTraceStage::ProfileIconReadback, resource, hr);
            if (hr == E_NOTIMPL) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if (hr != S_OK) return hr;
            if (actual_icon != current.path) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            // Attribute a successful API write whose marker commit may still
            // fail. No unrelated path may be adopted by readback or rollback.
            if (current.created_by_lifecycle) {
                pending_created_[slot(resource)] = true;
                pending_path_[slot(resource)] = target.path;
            }
            hr = api_.relocate_profile_icon(current.path, target.path);
            if (hr != S_OK) return hr;
        }
        if (current.present != target_present) {
            hr = set_api_presence(resource, target_present, target, phase, &expected_user); if (hr != S_OK) return hr;
            bool observed = false;
            hr = resource == Resource::profile ? api_.profile_presence_after_write(target_present, &observed) :
                api_.category_presence_after_write(resource, target_present, &observed);
            trace(phase, SharedStoreTraceStage::FinalReadback, resource, hr, hr == S_OK, observed);
            if (hr != S_OK) return hr;
            if (observed != target_present) {
                trace(phase, SharedStoreTraceStage::FinalVerification, resource,
                      HRESULT_FROM_WIN32(ERROR_INVALID_DATA), true, observed);
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            if (!observed) { pending_created_[slot(resource)] = false; pending_path_[slot(resource)].clear(); }
        }
        if ((resource == Resource::system_tray_category || resource == Resource::ui_element_category) && target_present) {
            bool relation_present = false;
            hr = api_.category_present(resource, &relation_present);
            trace(phase, SharedStoreTraceStage::CategoryReadbackAfterRegister, resource, hr,
                  hr == S_OK, relation_present);
            if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
            if (!relation_present) {
                trace(phase, SharedStoreTraceStage::FinalVerification, resource,
                      HRESULT_FROM_WIN32(ERROR_INVALID_DATA), true, false);
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }

            const DWORD required_capability = resource == Resource::system_tray_category ?
                kInstallerProfileCapsSysTraySupport : kInstallerProfileCapsUiElementEnabled;
            DWORD capabilities = 0;
            HRESULT capability_status = api_.profile_capabilities_with_required(required_capability,&capabilities);
            const HRESULT api_capability_status = capability_status;
            if (capability_status == S_FALSE) capability_status = E_FAIL;
            const bool caps_known = capability_status == S_OK;
            trace(phase, SharedStoreTraceStage::ProfileCapabilities, resource, api_capability_status,
                  false, false, caps_known, capabilities, required_capability);
            if (capability_status == S_OK &&
                (capabilities & required_capability) == 0)
                capability_status = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if (capability_status != S_OK) {
                // Compensate only the relation this transition added. An
                // already-present relation may be borrowed, so leave it intact
                // and report the missing capability to the caller.
                if (!relation_was_present) {
                    const HRESULT remove_status = api_.unregister_category(resource);
                    trace(phase == SharedStoreTracePhase::Apply ? SharedStoreTracePhase::ApplyCompensation :
                          SharedStoreTracePhase::RestoreCompensation,
                          SharedStoreTraceStage::UnregisterCategoryCompensation, resource, remove_status);
                    bool remains = true;
                    const HRESULT readback_status = api_.category_present(resource, &remains);
                    trace(phase == SharedStoreTracePhase::Apply ? SharedStoreTracePhase::ApplyCompensation :
                          SharedStoreTracePhase::RestoreCompensation,
                          SharedStoreTraceStage::CategoryReadbackAfterCompensation, resource, readback_status,
                          readback_status == S_OK, remains);
                    if (remove_status != S_OK || readback_status != S_OK || remains)
                        return FAILED(remove_status) ? remove_status :
                            FAILED(readback_status) ? readback_status :
                            HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                    pending_created_[slot(resource)] = false;
                    pending_path_[slot(resource)].clear();
                }
                return capability_status;
            }
        }
        if (target_present && target.created_by_lifecycle) {
            hr = write_marker_for(resource, target, phase);
            if (FAILED(hr)) {
                // A newly added TSF relation must not become unowned if its
                // durable marker cannot be committed. Compensate immediately;
                // if compensation fails, keep the pending state so transaction
                // rollback can retry under its compare-before-restore guard.
                if (!current.present && target_present) {
                    set_api_presence(resource, false, current, phase, nullptr, true);
                    bool still_present = true;
                    HRESULT state_status = resource == Resource::profile ? api_.profile_present(&still_present) : api_.category_present(resource, &still_present);
                    if (state_status == S_OK && !still_present) pending_created_[slot(resource)] = false;
                    OwnerMarker partial; HRESULT marker_status = markers_.read(resource, &partial);
                    if (marker_status == S_OK && partial.present && partial.owner == owner_) markers_.remove(resource);
                }
                return hr;
            }
            pending_created_[slot(resource)] = false; pending_path_[slot(resource)].clear();
        } else if (!target_present || !target.created_by_lifecycle) {
            if (marker.present) {
                hr = markers_.remove(resource);
                trace(phase, SharedStoreTraceStage::MarkerRemove, resource, hr);
                if (FAILED(hr)) return hr;
            }
            if (!target_present) pending_created_[slot(resource)] = false;
        }
        ResourceState verified; hr = read_uncached(resource, &verified);
        trace(phase, SharedStoreTraceStage::FinalReadback, resource, hr, hr == S_OK, verified.present);
        if (hr != S_OK) return hr;
        // New APIs verify independently read icon-file metadata; legacy APIs
        // retain their marker-only path policy and cannot relocate profiles.
        if (verified.present != target.present ||
            (target.present && verified.created_by_lifecycle != target.created_by_lifecycle) ||
            (!target.present && verified.created_by_lifecycle) ||
            (target.present && target.created_by_lifecycle && (verified.owner != target.owner || verified.path != target.path)))
            { trace(phase, SharedStoreTraceStage::FinalVerification, resource, HRESULT_FROM_WIN32(ERROR_INVALID_DATA), true, verified.present); return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
        if (resource == Resource::profile && target_present) {
            ProfileUserState verified_user;
            const bool user_known = observed_profile_user_state(verified, &verified_user);
            if (!user_known ||
                verified_user.enabled != expected_user.enabled ||
                verified_user.is_default != expected_user.is_default)
                { trace_user(phase, SharedStoreTraceStage::FinalUserStateVerification,
                             HRESULT_FROM_WIN32(ERROR_INVALID_DATA), user_known, verified_user.enabled, verified_user.is_default);
                  return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
        }
        const size_t user_bytes = resource == Resource::profile && target_present ? sizeof(uint32_t) + 2 : 0;
        if (verified.private_data.size() != expected_marker.size() + user_bytes ||
            !std::equal(expected_marker.begin(), expected_marker.end(), verified.private_data.begin()))
            { trace(phase, resource == Resource::profile ? SharedStoreTraceStage::FinalMarkerVerification :
                    SharedStoreTraceStage::FinalVerification, resource, HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
              return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
        trace(phase, SharedStoreTraceStage::FinalVerification, resource, S_OK, true, verified.present);
        if (verified_write) *verified_write = std::move(verified);
        return S_OK;
    }
public:
    WindowsTsfSharedStore(TsfSharedApi& api, OwnerMarkerStore& markers, std::wstring owner,
                          SharedStoreTrace* trace = nullptr,
                          ProfileRelocationPolicy policy = ProfileRelocationPolicy::owned_only)
        : api_(api), markers_(markers), owner_(std::move(owner)), trace_(trace), relocation_policy_(policy) {}
    HRESULT prepare_apply_targets(TransactionOperation operation, const InstallRequest& request,
        const ResourceStates& before, ResourceStates* desired) override {
        if (!desired) return E_POINTER;
        if (operation != TransactionOperation::install_or_repair ||
            relocation_policy_ != ProfileRelocationPolicy::package_borrowed_metadata) return S_OK;
        const auto& old = before[static_cast<size_t>(Resource::profile)];
        auto& target = (*desired)[static_cast<size_t>(Resource::profile)];
        if (!old.present || old.created_by_lifecycle) return S_OK;
        if (request.include_x86 || request.owner != owner_ || old.owner != owner_ || target != old ||
            !profile_icon::usable_path(request.icon_path)) return E_INVALIDARG;
        std::wstring actual_icon;
        const HRESULT hr = api_.profile_icon_path(&actual_icon);
        if (hr != S_OK) return hr;
        if (actual_icon != old.path) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        target.path = request.icon_path;
        return S_OK;
    }
    HRESULT read(Resource resource, ResourceState* state) override {
        if (!is_shared(resource)) return E_INVALIDARG;
        if (!state) return E_POINTER;
        const size_t index = slot(resource);
        valid_[index] = false;
        last_[index] = ResourceState{};
        ResourceState observed;
        const HRESULT hr = read_uncached(resource, &observed);
        if (hr != S_OK) { applied_valid_[index] = false; *state = ResourceState{}; return hr; }
        last_[index] = observed; valid_[index] = true;
        *state = std::move(observed);
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        if (!is_shared(resource)) return E_INVALIDARG;
        const size_t index = slot(resource);
        applied_valid_[index] = false;
        if (!valid_[index]) return E_UNEXPECTED;
        ResourceState current; HRESULT hr = read_uncached(resource, &current);
        trace(SharedStoreTracePhase::Apply, SharedStoreTraceStage::StateRevalidation, resource, hr, hr == S_OK, current.present);
        if (hr != S_OK) { valid_[index] = false; last_[index] = ResourceState{}; return hr; }
        if (current != last_[index]) { trace(SharedStoreTracePhase::Apply, SharedStoreTraceStage::StateRevalidation, resource, HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), true, current.present); return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH); }
        ResourceState verified_write;
        hr = transition(resource, current, desired, SharedStoreTracePhase::Apply, &verified_write);
        ResourceState after; HRESULT observed = read_uncached(resource, &after);
        if (observed == S_OK) last_[index] = after;
        else { valid_[index] = false; last_[index] = ResourceState{}; }
        // New owner-marker and user-state bytes are produced by this store.
        // Retain their exact readback rather than comparing them with the
        // pre-write opaque bytes carried by the transaction's desired state.
        if (hr == S_OK && observed == S_OK) {
            // A later read must match the already validated write. Never
            // adopt a foreign marker or changed user state as a new baseline.
            if (after != verified_write) {
                trace(SharedStoreTracePhase::Apply, SharedStoreTraceStage::PostApplyVerification,
                      resource, HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            applied_target_[index] = desired;
            applied_state_[index] = std::move(verified_write);
            applied_valid_[index] = true;
        }
        return hr != S_OK ? hr : observed;
    }
    HRESULT project_apply_state(Resource resource, const ResourceState& state, ResourceState* out) const override {
        if (!out) return E_POINTER;
        *out = ResourceState{};
        if (!is_shared(resource)) return E_INVALIDARG;
        // Only recognized legacy marker/LSU1 bytes may enter this apply domain.
        // Unknown opaque payloads, including PDE1, fail closed.
        const auto& bytes = state.private_data;
        size_t prefix_size = 0;
        if (!bytes.empty()) {
            OwnerMarker marker;
            if (decode_marker_prefix(bytes, &marker)) {
                std::vector<unsigned char> canonical;
                encode_owner_marker(marker, &canonical);
                if (canonical != marker.encoded) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                prefix_size = marker.encoded.size();
            }
            const size_t suffix = bytes.size() - prefix_size;
            if (suffix != 0) {
                if (resource != Resource::profile || suffix != sizeof(uint32_t) + 2)
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                uint32_t magic = 0;
                std::memcpy(&magic, bytes.data() + prefix_size, sizeof(magic));
                if (magic != kProfileUserStateMagic || bytes[prefix_size + sizeof(magic)] > 1 ||
                    bytes[prefix_size + sizeof(magic) + 1] > 1)
                    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            } else if (prefix_size == 0) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        *out = state;
        // Versioned journal projection only. CAS/verify/restore retain full bytes.
        out->private_data = {'L','K','S','A','P','P','L','Y',1,0,0,0};
        return S_OK;
    }
    HRESULT verify_applied(Resource resource, const ResourceState& desired,
                           const ResourceState& observed) const override {
        if (!is_shared(resource)) return E_INVALIDARG;
        const size_t index = slot(resource);
        if (!valid_[index] || !applied_valid_[index]) return E_UNEXPECTED;
        return desired == applied_target_[index] && observed == applied_state_[index] &&
            observed == last_[index] ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    HRESULT restore(Resource resource, const ResourceState& snapshot) override {
        if (!is_shared(resource)) return E_INVALIDARG;
        const size_t index = slot(resource);
        applied_valid_[index] = false;
        ResourceState current; HRESULT hr = read_uncached(resource, &current);
        trace(SharedStoreTracePhase::Restore, SharedStoreTraceStage::StateRevalidation, resource, hr, hr == S_OK, current.present);
        if (hr != S_OK) { valid_[index] = false; last_[index] = ResourceState{}; return hr; }
        if (!valid_[index] || current != last_[index]) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        hr = transition(resource, current, snapshot, SharedStoreTracePhase::Restore);
        ResourceState after; HRESULT observed = read_uncached(resource, &after);
        if (observed == S_OK) last_[index] = after;
        else { valid_[index] = false; last_[index] = ResourceState{}; }
        return hr != S_OK ? hr : observed;
    }
    bool failure_may_have_mutated(Resource resource, HRESULT status) const override {
        if (!is_shared(resource)) return true;
        return status != HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) && status != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) &&
               status != E_INVALIDARG &&
               !(resource == Resource::profile && status == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    }
    HRESULT category_supported(Resource resource, DWORD capability, bool* supported) {
        if (!supported) return E_POINTER;
        *supported = false;
        bool relation_present = false;
        HRESULT hr = api_.category_present(resource, &relation_present);
        if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
        DWORD capabilities = 0;
        hr = api_.profile_capabilities_with_required(capability,&capabilities);
        if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
        *supported = relation_present &&
            (capabilities & capability) != 0;
        return S_OK;
    }
    HRESULT system_tray_supported(bool* supported) {
        return category_supported(Resource::system_tray_category,
            kInstallerProfileCapsSysTraySupport, supported);
    }
    HRESULT verify_install_health() override {
        bool supported = false;
        const HRESULT hr = category_supported(Resource::system_tray_category,
            kInstallerProfileCapsSysTraySupport, &supported);
        if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
        if (!supported) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        const HRESULT ui_status = category_supported(Resource::ui_element_category,
            kInstallerProfileCapsUiElementEnabled, &supported);
        if (ui_status != S_OK) return FAILED(ui_status) ? ui_status : E_FAIL;
        return supported ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
};

} // namespace boshiamy::installer
#endif
