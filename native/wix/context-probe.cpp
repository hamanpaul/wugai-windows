#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <sddl.h>
#include <string>
#include <vector>

// Read-only MSI custom-action diagnostic. Never writes registry or changes TSF.
extern "C" __declspec(dllexport) UINT __stdcall ProbeContext(MSIHANDLE install) {
    HANDLE token=nullptr;
    bool impersonating=true;
    if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token)) {
        if(GetLastError()!=ERROR_NO_TOKEN)return ERROR_INSTALL_FAILURE;
        impersonating=false;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return ERROR_INSTALL_FAILURE;
    }
    DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    if(!bytes){CloseHandle(token);return ERROR_INSTALL_FAILURE;}
    std::vector<unsigned char> buffer(bytes);
    if(!GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes)){CloseHandle(token);return ERROR_INSTALL_FAILURE;}
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid)){CloseHandle(token);return ERROR_INSTALL_FAILURE;}
    TOKEN_ELEVATION elevation{};DWORD size=0;
    const BOOL gotElevation=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size);
    DWORD tokenSession=0;DWORD tokenSessionSize=0;
    const BOOL gotTokenSession=GetTokenInformation(token,TokenSessionId,&tokenSession,sizeof(tokenSession),&tokenSessionSize);
    DWORD session=0;const BOOL gotSession=ProcessIdToSessionId(GetCurrentProcessId(),&session);
    CloseHandle(token);
    if(!gotElevation||!gotTokenSession||!gotSession){LocalFree(sid);return ERROR_INSTALL_FAILURE;}
    HKEY key=nullptr;
    const LONG found=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\Profile",0,KEY_READ,&key);
    if(key)RegCloseKey(key);
    if(found!=ERROR_SUCCESS&&found!=ERROR_FILE_NOT_FOUND&&found!=ERROR_PATH_NOT_FOUND){LocalFree(sid);return ERROR_INSTALL_FAILURE;}
    std::wstring line=L"LiuKai MSI context: token="+std::wstring(impersonating?L"thread":L"process")+L" sid="+std::wstring(sid)+L" elevated="+(elevation.TokenIsElevated?L"true":L"false")+
        L" token_session="+std::to_wstring(tokenSession)+L" process_session="+std::to_wstring(session)+L" hkcu_marker="+(found==ERROR_SUCCESS?L"present":L"absent");
    LocalFree(sid);
    MSIHANDLE record=MsiCreateRecord(1);if(!record)return ERROR_INSTALL_FAILURE;
    UINT result=MsiRecordSetStringW(record,0,line.c_str());
    if(result==ERROR_SUCCESS)MsiProcessMessage(install,INSTALLMESSAGE_INFO,record);
    MsiCloseHandle(record);
    return result==ERROR_SUCCESS?ERROR_SUCCESS:ERROR_INSTALL_FAILURE;
}
