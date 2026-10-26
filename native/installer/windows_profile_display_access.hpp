#pragma once
#include "profile_display_backend.hpp"
#include "profile_display_mutation.hpp"
#include "profile_display_registry.hpp"

namespace boshiamy::installer::profile_display_windows {
using Snapshot = profile_display_state::DisplaySnapshot;
using Authority = std::function<HRESULT(const Snapshot&, const Snapshot&)>;
using Health = std::function<HRESULT(const Snapshot&)>;
// Handles and profile reader are borrowed. The composition root must bind the
// hive/view/key, shared-key policy, writer mutex, authority and resource health
// to the same validated profile/package. This adapter never infers HKLM
// authority from a per-user COM registration and never opens product keys.
class WindowsDisplayAccess final : public profile_display_backend::DisplayAccess,
                                   private profile_display_mutation::RawAccess {
    profile_display::ProfileDisplayBackend& profiles_;
    HKEY views_[2];
    bool shared_;
    std::wstring writer_mutex_;
    Authority authority_;
    Health health_;
    static HRESULT error(DWORD status) { return status ? HRESULT_FROM_WIN32(status) : E_FAIL; }
    HRESULT read_values(Snapshot* out) {
        Snapshot value;
        HRESULT status = profile_display_registry::read(views_[0], profile_display_registry::DisplayValue::Description,
                                                        &value.descriptions.registry64);
        if (status != S_OK) return status;
        status = profile_display_registry::read(views_[1], profile_display_registry::DisplayValue::Description,
                                               &value.descriptions.registry32);
        if (status != S_OK) return status;
        status = profile_display_registry::read(views_[0], profile_display_registry::DisplayValue::DisplayDescription,
                                               &value.display_binding.registry64);
        if (status != S_OK) return status;
        status = profile_display_registry::read(views_[1], profile_display_registry::DisplayValue::DisplayDescription,
                                               &value.display_binding.registry32);
        if (status != S_OK) return status;
        out->descriptions = std::move(value.descriptions);
        out->display_binding = std::move(value.display_binding);
        return S_OK;
    }
    bool shared_views() const override { return shared_; }
    HRESULT conditional_write(profile_display_mutation::Field field,
                              const profile_display_mutation::RawValue& expected,
                              const profile_display_mutation::RawValue& target) override {
        const bool view32 = field == profile_display_mutation::Field::description32 ||
                            field == profile_display_mutation::Field::binding32;
        const bool binding = field == profile_display_mutation::Field::binding64 ||
                             field == profile_display_mutation::Field::binding32;
        return profile_display_registry::conditional_write(views_[view32 ? 1 : 0], binding
            ? profile_display_registry::DisplayValue::DisplayDescription
            : profile_display_registry::DisplayValue::Description, expected, target);
    }
    struct WriterLock {
        HANDLE handle = nullptr;
        bool acquired = false;
        ~WriterLock() {
            if (acquired) ReleaseMutex(handle);
            if (handle) CloseHandle(handle);
        }
    };
public:
    WindowsDisplayAccess(profile_display::ProfileDisplayBackend& profiles, HKEY view64, HKEY view32,
                         bool shared, std::wstring writer_mutex, Authority authority = {}, Health health = {})
        : profiles_(profiles), views_{view64, view32}, shared_(shared), writer_mutex_(std::move(writer_mutex)),
          authority_(std::move(authority)), health_(std::move(health)) {}
    WindowsDisplayAccess(const WindowsDisplayAccess&) = delete;
    WindowsDisplayAccess& operator=(const WindowsDisplayAccess&) = delete;
    HRESULT capture(Snapshot* out) override {
        if (!out) return E_POINTER;
        *out = {};
        try {
            Snapshot first, second;
            HRESULT status = profiles_.read(&first.profile);
            if (status != S_OK) return status;
            status = read_values(&first);
            if (status != S_OK) return status;
            status = profiles_.read(&second.profile);
            if (status != S_OK) return status;
            status = read_values(&second);
            if (status != S_OK) return status;
            if (!profile_display_state::equal(first, second)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            *out = std::move(second);
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }
    HRESULT authorize(const Snapshot& expected, const Snapshot& target) override {
        return authority_ ? authority_(expected, target) : E_ACCESSDENIED;
    }
    HRESULT exchange(const Snapshot& expected, const Snapshot& target) override {
        if (writer_mutex_.empty() || writer_mutex_.find(L'\0') != std::wstring::npos || writer_mutex_.size() >= MAX_PATH)
            return E_INVALIDARG;
        WriterLock lock;
        lock.handle = CreateMutexW(nullptr, FALSE, writer_mutex_.c_str());
        if (!lock.handle) return error(GetLastError());
        const DWORD waited = WaitForSingleObject(lock.handle, 0);
        if (waited == WAIT_ABANDONED) {
            ReleaseMutex(lock.handle);
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); // Require recovery inspection first.
        }
        if (waited != WAIT_OBJECT_0) return waited == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY) : error(GetLastError());
        lock.acquired = true;
        return profile_display_mutation::exchange(*this, expected, target);
    }
    HRESULT verify_target(const Snapshot& target) override {
        Snapshot observed;
        HRESULT status = capture(&observed);
        if (status != S_OK) return status;
        if (!profile_display_state::equal(observed, target)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        status = authorize(target, target);
        if (status != S_OK) return status;
        if (!health_) return E_NOTIMPL;
        const HRESULT health = health_(target);
        const HRESULT readback = capture(&observed);
        if (health != S_OK) return health;
        if (readback != S_OK) return readback;
        return profile_display_state::equal(observed, target) ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
};
}
