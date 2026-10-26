#ifndef LIUKAI_INSTALLER_DEPLOYMENT_ACCESS_PREFLIGHT_HPP
#define LIUKAI_INSTALLER_DEPLOYMENT_ACCESS_PREFLIGHT_HPP

#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace boshiamy::installer::deployment_access {

inline constexpr const wchar_t* kCategoryParent =
    L"SOFTWARE\\Microsoft\\CTF\\TIP\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\Category\\Category";
inline constexpr const wchar_t* kItemParent =
    L"SOFTWARE\\Microsoft\\CTF\\TIP\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\Category\\Item\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}";
inline constexpr const wchar_t* kStopAncestor = L"SOFTWARE\\Microsoft\\CTF\\TIP";
inline constexpr REGSAM kRequestedAccess = KEY_CREATE_SUB_KEY | KEY_SET_VALUE | KEY_WOW64_64KEY;

inline bool canonical_sid(const std::wstring& text) {
    if (text.empty() || text.size() > 184 || text.find(L'\0') != std::wstring::npos) return false;
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(text.c_str(), &parsed) || !parsed) return false;
    LPWSTR normalized = nullptr;
    const bool valid = IsValidSid(parsed) && ConvertSidToStringSidW(parsed, &normalized) && normalized && text == normalized;
    if (normalized) LocalFree(normalized);
    LocalFree(parsed);
    return valid;
}

struct Result {
    std::wstring expected_sid;
    std::wstring actual_sid;
    DWORD token_status = ERROR_GEN_FAILURE;
    bool identity_match = false;
    bool category_checked = false;
    std::wstring category_probed_path;
    DWORD category_open_status = ERROR_GEN_FAILURE;
    bool item_checked = false;
    std::wstring item_probed_path;
    DWORD item_open_status = ERROR_GEN_FAILURE;
    bool passed() const {
        return token_status == ERROR_SUCCESS && identity_match && category_checked &&
               category_open_status == ERROR_SUCCESS && item_checked && item_open_status == ERROR_SUCCESS;
    }
};

class Api {
public:
    virtual ~Api() = default;
    virtual DWORD process_user_sid(std::wstring* sid) = 0;
    virtual DWORD open_existing_hklm64(const wchar_t* path, REGSAM requested) = 0;
};

struct ExistingAncestor {
    std::wstring path;
    DWORD status = ERROR_GEN_FAILURE;
};

inline ExistingAncestor probe_existing_ancestor(const wchar_t* target, Api& api) {
    std::wstring path(target);
    const std::wstring stop(kStopAncestor);
    for (;;) {
        const DWORD status = api.open_existing_hklm64(path.c_str(), kRequestedAccess);
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
            return {path, status};
        if (path == stop) return {path, status};
        if (path.size() <= stop.size() || path.compare(0, stop.size(), stop) != 0 ||
            path[stop.size()] != L'\\') return {path, ERROR_INVALID_DATA};
        const auto separator = path.find_last_of(L'\\');
        if (separator == std::wstring::npos) return {path, ERROR_INVALID_DATA};
        path.resize(separator);
    }
}

inline Result check(const std::wstring& expected, Api& api) {
    Result result;
    result.expected_sid = expected;
    if (!canonical_sid(expected)) { result.token_status = ERROR_INVALID_SID; return result; }
    result.token_status = api.process_user_sid(&result.actual_sid);
    if (result.token_status != ERROR_SUCCESS || !canonical_sid(result.actual_sid)) {
        if (result.token_status == ERROR_SUCCESS) result.token_status = ERROR_INVALID_SID;
        return result;
    }
    result.identity_match = result.actual_sid == expected;
    if (!result.identity_match) return result; // Never probe HKLM for the wrong user's token.
    result.category_checked = true;
    const auto category = probe_existing_ancestor(kCategoryParent, api);
    result.category_probed_path = category.path;
    result.category_open_status = category.status;
    result.item_checked = true;
    const auto item = probe_existing_ancestor(kItemParent, api);
    result.item_probed_path = item.path;
    result.item_open_status = item.status;
    return result;
}

class WindowsApi final : public Api {
public:
    DWORD process_user_sid(std::wstring* sid) override {
        if (!sid) return ERROR_INVALID_PARAMETER;
        sid->clear();
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        DWORD status = GetLastError();
        if (status != ERROR_INSUFFICIENT_BUFFER || size < sizeof(TOKEN_USER) || size > 65536) {
            CloseHandle(token);
            return status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
        }
        std::vector<ULONG_PTR> buffer((size + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
        if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
            status = GetLastError(); CloseHandle(token); return status;
        }
        CloseHandle(token);
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        if (!user->User.Sid || !IsValidSid(user->User.Sid)) return ERROR_INVALID_SID;
        LPWSTR converted = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &converted)) return GetLastError();
        if (!converted) return ERROR_INVALID_SID;
        struct LocalString {
            LPWSTR value;
            ~LocalString() { LocalFree(value); }
        } owned{converted};
        *sid = owned.value;
        return ERROR_SUCCESS;
    }
    DWORD open_existing_hklm64(const wchar_t* path, REGSAM requested) override {
        HKEY key = nullptr;
        const LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, requested, &key);
        if (key) RegCloseKey(key);
        return static_cast<DWORD>(status);
    }
};

} // namespace boshiamy::installer::deployment_access
#endif
