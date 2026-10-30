#pragma once
#include <windows.h>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "failure-collector.hpp"

namespace liukai::wix_legacy_recovery {
namespace files=boshiamy::installer::files;
struct LockedFiles {
    std::vector<HANDLE> handles;
    ~LockedFiles(){for(auto h:handles)CloseHandle(h);}
    bool lock_verified(const std::filesystem::path& file,const std::string& hash) {
        if(!files::detail::absolute_no_reparse(file,true))return false;
        HANDLE h=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        handles.push_back(h);return files::detail::sha256_file(file)==hash;
    }
};
inline bool decode_proof(const std::string& proof,std::map<std::string,std::string>& out){
    out.clear();size_t at=0;
    while(at<proof.size()){
        const auto end=proof.find(';',at);const auto row=proof.substr(at,end==std::string::npos?end:end-at);const auto eq=row.find('=');
        if(eq==std::string::npos||row.find('=',eq+1)!=std::string::npos)return false;
        const auto path=row.substr(0,eq),hash=row.substr(eq+1);
        if(!files::detail::safe_relative(path)||!files::detail::valid_hash(hash)||!out.emplace(path,hash).second)return false;
        if(end==std::string::npos)break;at=end+1;
    }
    static const std::set<std::string> required={"LiuKaiRegister.exe","support/recover-legacy-profile.ps1","support/legacy-prepared-root.ps1","support/legacy-recovery-policy.ps1","support/legacy-recovery-observe.ps1","support/legacy-recovery-transaction.ps1","support/legacy-recovery-adapter.ps1","support/legacy-recovery-backup.ps1","support/legacy-recovery-registry.ps1","support/legacy-owner-marker.ps1","support/legacy-history-probe.exe","support/collect-install-diagnostics.ps1"};
    if(out.size()!=required.size())return false;
    for(const auto& key:required)if(out.find(key)==out.end())return false;
    return true;
}
inline bool verify_helpers(const std::filesystem::path& root,const std::string& version,
                           const std::string& manifest_hash,const std::string& helper_hash,
                           const std::string& proof,LockedFiles& locks){
    if(!files::detail::safe_label(version)||!files::detail::valid_hash(manifest_hash)||!files::detail::valid_hash(helper_hash)||
       !files::detail::absolute_no_reparse(root,true))return false;
    std::map<std::string,std::string> extras;if(!decode_proof(proof,extras))return false;
    for(const auto& pair:extras){const auto path=root/std::filesystem::path(pair.first);if(!locks.lock_verified(path,pair.second))return false;}
    const auto manifest=root/L"versions"/std::filesystem::path(version)/L"package-manifest.json";
    return locks.lock_verified(manifest,manifest_hash)&&locks.lock_verified(root/L"LiuKaiInstaller.exe",helper_hash);
}
inline HRESULT present(HKEY hive,const wchar_t* path,bool& found){
    HKEY key=nullptr;const LONG status=RegOpenKeyExW(hive,path,0,KEY_READ|KEY_WOW64_64KEY,&key);
    if(status==ERROR_SUCCESS){RegCloseKey(key);found=true;return S_OK;}
    if(status==ERROR_FILE_NOT_FOUND||status==ERROR_PATH_NOT_FOUND){found=false;return S_OK;}
    return HRESULT_FROM_WIN32(status);
}
inline HRESULT orphan_present(HKEY hive,bool& found){
    bool profile=false,marker=false;
    HRESULT hr=present(hive,L"Software\\Microsoft\\CTF\\TIP\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\LanguageProfile\\0x00000404\\{7254A21F-6EB1-4217-A42C-C27301175D90}",profile);
    if(hr!=S_OK)return hr;
    hr=present(hive,L"Software\\LiuKai\\Installer\\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\\Profile",marker);
    if(hr!=S_OK)return hr;
    if(profile!=marker)return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    found=profile;return S_OK;
}
inline HRESULT orphan_present(bool& found){return orphan_present(HKEY_CURRENT_USER,found);}
inline DWORD run_script(const std::filesystem::path& root,const std::wstring& sid,const std::wstring& version,
                        const std::string& manifest_hash,const std::string& helper_hash,
                        const std::string& history_hash,const std::string& proof,
                        const std::filesystem::path& log_dir,std::wstring* report_path){
    wchar_t system[MAX_PATH+1]{};const UINT n=GetSystemDirectoryW(system,MAX_PATH);if(!n||n>=MAX_PATH)return ERROR_PATH_NOT_FOUND;
    const auto shell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    if(!files::detail::absolute_no_reparse(shell,true))return ERROR_PATH_NOT_FOUND;
    using liukai::wix_diagnostics::quote_argument;
    auto widen=[](const std::string& text){return std::wstring(text.begin(),text.end());};
    const auto support=root/L"support";
    std::wstring command=quote_argument(shell.wstring())+L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quote_argument((support/L"recover-legacy-profile.ps1").wstring())+
        L" -Cli "+quote_argument((root/L"LiuKaiInstaller.exe").wstring())+L" -CliSha256 "+quote_argument(widen(helper_hash))+
        L" -HistoryProbe "+quote_argument((support/L"legacy-history-probe.exe").wstring())+L" -HistorySha256 "+quote_argument(widen(history_hash))+
        L" -ExpectedUserSid "+quote_argument(sid)+L" -Execute -PreparedVersion "+quote_argument(version)+
        L" -PreparedManifestSha256 "+quote_argument(widen(manifest_hash))+L" -PreparedExtraHashes "+quote_argument(widen(proof));
    std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(L'\0');
    if(!report_path||!boshiamy::installer::durable_cli::detail::ensure_directory_chain(log_dir))return ERROR_ACCESS_DENIED;
    std::string token;if(!boshiamy::installer::durable_cli::detail::make_token(&token))return ERROR_GEN_FAILURE;
    const auto log=log_dir/(L"wix-recovery-"+std::wstring(token.begin(),token.end())+L".log");
    SECURITY_ATTRIBUTES sa{};sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
    HANDLE output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE)return GetLastError();
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input==INVALID_HANDLE_VALUE){const DWORD error=GetLastError();CloseHandle(output);return error;}
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=output;si.hStdError=output;si.hStdInput=input;
    PROCESS_INFORMATION pi{};*report_path=log.wstring();
    auto env=liukai::wix_diagnostics::powershell_environment(shell);
    if(env.empty()){CloseHandle(input);CloseHandle(output);return ERROR_NOT_ENOUGH_MEMORY;}
    const BOOL started=CreateProcessW(shell.c_str(),buffer.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,env.data(),nullptr,&si,&pi);
    const DWORD startError=started?ERROR_SUCCESS:GetLastError();CloseHandle(input);CloseHandle(output);
    if(!started)return startError;
    // Never kill a process that may be between registry writes. Its durable
    // backup is useful after interruption, but a timeout must not create one.
    const DWORD waited=WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=ERROR_INSTALL_FAILURE;
    if(waited==WAIT_OBJECT_0){if(!GetExitCodeProcess(pi.hProcess,&code))code=GetLastError();}else code=GetLastError();
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code;
}
}