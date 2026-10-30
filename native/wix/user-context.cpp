#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <sddl.h>
#include <wtsapi32.h>
#include <string>
#include <vector>

// Reject over-the-shoulder elevation: the elevated process account must also
// be the account logged into the desktop session hosting this MSI action.
static bool desktop_session_matches(PSID processSid) {
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||session==0)return false;
    LPWSTR rawUser=nullptr,rawDomain=nullptr;DWORD userBytes=0,domainBytes=0;
    const bool gotUser=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSUserName,&rawUser,&userBytes)!=FALSE;
    const bool gotDomain=WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSDomainName,&rawDomain,&domainBytes)!=FALSE;
    std::wstring user=gotUser&&rawUser?rawUser:L"",domain=gotDomain&&rawDomain?rawDomain:L"";
    if(rawUser)WTSFreeMemory(rawUser);if(rawDomain)WTSFreeMemory(rawDomain);
    if(!gotUser||!gotDomain||user.empty()||domain.empty())return false;
    const std::wstring account=domain+L"\\"+user;
    DWORD sidBytes=0,domainChars=0;SID_NAME_USE use{};
    LookupAccountNameW(nullptr,account.c_str(),nullptr,&sidBytes,nullptr,&domainChars,&use);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||!sidBytes||!domainChars)return false;
    std::vector<BYTE> sid(sidBytes);std::vector<wchar_t> resolvedDomain(domainChars);
    if(!LookupAccountNameW(nullptr,account.c_str(),sid.data(),&sidBytes,resolvedDomain.data(),&domainChars,&use))return false;
    return IsValidSid(sid.data())&&EqualSid(processSid,sid.data());
}

// A machine installation must never register the input method into SYSTEM's
// HKCU or silently switch to another process identity between MSI actions.
// This check is only one prerequisite for the registration transaction.
bool elevated_process_user(std::wstring& sid) {
    HANDLE thread=nullptr;
    if (OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&thread)) {
        CloseHandle(thread);
        return false; // HKCU and the legacy backend use the process context.
    }
    if (GetLastError()!=ERROR_NO_TOKEN) return false;
    HANDLE token=nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
    TOKEN_ELEVATION elevation{}; DWORD bytes=0;
    if (!GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes) || !elevation.TokenIsElevated) {
        CloseHandle(token); return false;
    }
    GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    if (!bytes) { CloseHandle(token); return false; }
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token,TokenUser,storage.data(),bytes,&bytes)) { CloseHandle(token); return false; }
    CloseHandle(token);
    auto* user=reinterpret_cast<TOKEN_USER*>(storage.data());
    if (!IsValidSid(user->User.Sid) || IsWellKnownSid(user->User.Sid,WinLocalSystemSid) ||
        IsWellKnownSid(user->User.Sid,WinLocalServiceSid) || IsWellKnownSid(user->User.Sid,WinNetworkServiceSid) ||
        !desktop_session_matches(user->User.Sid)) return false;
    LPWSTR value=nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid,&value)) return false;
    sid=value; LocalFree(value); return true;
}
bool property(MSIHANDLE install, const wchar_t* name, std::wstring& value) {
    wchar_t empty[1]{}; DWORD size=0; UINT status=MsiGetPropertyW(install,name,empty,&size);
    if (status!=ERROR_MORE_DATA && status!=ERROR_SUCCESS) return false;
    std::vector<wchar_t> buffer(static_cast<size_t>(size)+1);
    DWORD capacity=size+1;
    if (MsiGetPropertyW(install,name,buffer.data(),&capacity)!=ERROR_SUCCESS) return false;
    value.assign(buffer.data(),capacity); return true;
}
extern "C" __declspec(dllexport) UINT __stdcall CaptureUserContext(MSIHANDLE install) {
    std::wstring sid;
    if (!elevated_process_user(sid)) return ERROR_INSTALL_FAILURE;
    return MsiSetPropertyW(install,L"ValidateUserContext",sid.c_str())==ERROR_SUCCESS ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateUserContext(MSIHANDLE install) {
    std::wstring current, expected;
    if (!property(install,L"CustomActionData",expected) || !elevated_process_user(current) || expected!=current)
        return ERROR_INSTALL_FAILURE;
    return ERROR_SUCCESS;
}
