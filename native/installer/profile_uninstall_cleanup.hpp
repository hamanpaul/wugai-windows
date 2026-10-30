#pragma once
#ifdef _WIN32
#include <windows.h>
#include <string>
#include <utility>

namespace boshiamy::installer::profile_uninstall {
struct UserState {
    bool present = false;
    DWORD enabled = 0;
    bool operator==(const UserState& other) const { return present == other.present && enabled == other.enabled; }
    bool operator!=(const UserState& other) const { return !(*this == other); }
};
class Store {
public:
    virtual ~Store() = default;
    virtual HRESULT machine_present(bool*) = 0;
    // Only absence or a leaf containing exactly Enable REG_DWORD 0/1 is valid.
    virtual HRESULT read_user(UserState*) = 0;
    virtual HRESULT remove_user(const UserState& expected) = 0;
};
inline HRESULT finish(Store& store, const UserState& before) {
    bool machine = true;
    HRESULT status = store.machine_present(&machine);
    if (status != S_OK) return status;
    if (machine) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    UserState after;
    status = store.read_user(&after);
    if (status != S_OK) return status;
    if (!after.present) return S_OK;
    if (after != before) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    status = store.remove_user(before);
    if (status != S_OK) return status;
    status = store.read_user(&after);
    if (status != S_OK) return status;
    if (after.present) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    status = store.machine_present(&machine);
    return status != S_OK ? status : machine ? HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) : S_OK;
}

class WindowsStore final : public Store {
    std::wstring path_;
    static HRESULT win32(LONG status) { return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }
    static bool missing(LONG status) { return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND; }
public:
    explicit WindowsStore(std::wstring path) : path_(std::move(path)) {}
    HRESULT machine_present(bool* present) override {
        if (!present) return E_POINTER;
        *present = false;
        HKEY key = nullptr;
        const LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path_.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (missing(status)) return S_OK;
        if (status != ERROR_SUCCESS) return win32(status);
        RegCloseKey(key); *present = true; return S_OK;
    }
    HRESULT read_user(UserState* state) override {
        if (!state) return E_POINTER;
        *state = {};
        HKEY key = nullptr;
        LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
        if (missing(status)) return S_OK;
        if (status != ERROR_SUCCESS) return win32(status);
        DWORD children = 0, values = 0;
        status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &children, nullptr, nullptr,
            &values, nullptr, nullptr, nullptr, nullptr);
        DWORD kind = 0, enabled = 0, size = sizeof(enabled);
        if (status == ERROR_SUCCESS && children == 0 && values == 1)
            status = RegQueryValueExW(key, L"Enable", nullptr, &kind, reinterpret_cast<BYTE*>(&enabled), &size);
        else if (status == ERROR_SUCCESS) status = ERROR_INVALID_DATA;
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) return win32(status);
        if (kind != REG_DWORD || size != sizeof(DWORD) || enabled > 1)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        state->present = true; state->enabled = enabled; return S_OK;
    }
    HRESULT remove_user(const UserState& expected) override {
        UserState current;
        HRESULT status = read_user(&current);
        if (status != S_OK) return status;
        if (!expected.present || current != expected) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        bool machine = true;
        status = machine_present(&machine);
        if (status != S_OK || machine) return status != S_OK ? status : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        // No recursive deletion and no parent cleanup: other language profiles
        // and user settings remain outside this one product/profile leaf.
        return win32(RegDeleteKeyExW(HKEY_CURRENT_USER, path_.c_str(), KEY_WOW64_64KEY, 0));
    }
};
} // namespace boshiamy::installer::profile_uninstall
#endif
