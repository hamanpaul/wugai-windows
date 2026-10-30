#pragma once
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include "../installer/file_deployment.hpp"
#include "../installer/durable_cli_session.hpp"

namespace liukai::wix_diagnostics {
// Windows command-line quoting; no shell evaluates these arguments.
inline std::wstring quote_argument(const std::wstring& input) {
    std::wstring out=L"\""; size_t slashes=0;
    for(wchar_t ch:input){
        if(ch==L'\\'){++slashes;continue;}
        if(ch==L'\"'){out.append(slashes*2+1,L'\\');out.push_back(ch);slashes=0;continue;}
        out.append(slashes,L'\\');slashes=0;out.push_back(ch);
    }
    out.append(slashes*2,L'\\');out.push_back(L'\"');return out;
}
inline std::vector<wchar_t> powershell_environment(const std::filesystem::path& shell){
    LPWCH block=GetEnvironmentStringsW();if(!block)return {};
    std::vector<std::wstring> entries;std::wstring old;bool found=false;
    for(const wchar_t* p=block;*p;){std::wstring value(p);p+=value.size()+1;
        if(_wcsnicmp(value.c_str(),L"PSModulePath=",13)==0){old=value.substr(13);found=true;}else entries.push_back(std::move(value));}
    FreeEnvironmentStringsW(block);
    auto modules=(shell.parent_path()/L"Modules").wstring();
    entries.push_back(L"PSModulePath="+modules+((found&&!old.empty())?L";"+old:L""));
    std::sort(entries.begin(),entries.end(),[](const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> result;for(const auto& entry:entries){result.insert(result.end(),entry.begin(),entry.end());result.push_back(L'\0');}
    result.push_back(L'\0');return result;
}
inline bool parse_data(const std::wstring& input,std::wstring& sid,std::wstring& root,std::wstring& log){
    const auto first=input.find(L'|'),second=first==std::wstring::npos?first:input.find(L'|',first+1);
    if(first==std::wstring::npos||second==std::wstring::npos||input.find(L'|',second+1)!=std::wstring::npos)return false;
    sid=input.substr(0,first);root=input.substr(first+1,second-first-1);log=input.substr(second+1);
    return !sid.empty()&&!root.empty()&&input.find(L'\0')==std::wstring::npos;
}
// Called only for the known MSI product and verified initiating user. The
// script stays in the MSI-owned Program Files directory and is not copied to a
// writable temporary executable path. Errors are reported to MSI; rollback
// itself is never blocked on diagnostic collection.
inline DWORD run_script(const std::filesystem::path& root,const std::filesystem::path& script,
                        const std::filesystem::path& output,const std::wstring& log,const std::string& expected_hash,
                        const std::wstring& stage=L"",const std::wstring& hresult=L"") {
    namespace files=boshiamy::installer::files;
    namespace durable=boshiamy::installer::durable_cli;
    if(!files::detail::absolute_no_reparse(script,true)||files::detail::sha256_file(script)!=expected_hash)return ERROR_CRC;
    if(!durable::detail::ensure_directory_chain(output))return ERROR_ACCESS_DENIED;
    wchar_t system[MAX_PATH+1]{};const UINT n=GetSystemDirectoryW(system,MAX_PATH);
    if(!n||n>=MAX_PATH)return ERROR_PATH_NOT_FOUND;
    const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    if(!files::detail::absolute_no_reparse(powershell,true))return ERROR_PATH_NOT_FOUND;
    std::wstring command=quote_argument(powershell.wstring())+L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+quote_argument(script.wstring())+L" -OutputDirectory "+quote_argument(output.wstring())+L" -InstallRoot "+quote_argument(root.wstring());
    if(!log.empty())command+=L" -MsiLog "+quote_argument(log);
    if(!stage.empty()&&!hresult.empty())command+=L" -FailureStage "+quote_argument(stage)+L" -FailureHResult "+quote_argument(hresult);
    std::vector<wchar_t> writable(command.begin(),command.end());writable.push_back(L'\0');
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    auto env=powershell_environment(powershell);if(env.empty())return ERROR_NOT_ENOUGH_MEMORY;
    if(!CreateProcessW(powershell.c_str(),writable.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,env.data(),nullptr,&si,&pi))return GetLastError();
    const DWORD waited=WaitForSingleObject(pi.hProcess,60000);DWORD code=ERROR_INSTALL_FAILURE;
    if(waited==WAIT_OBJECT_0){if(!GetExitCodeProcess(pi.hProcess,&code))code=GetLastError();}
    else {if(waited==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,ERROR_TIMEOUT);WaitForSingleObject(pi.hProcess,5000);code=ERROR_TIMEOUT;}else code=GetLastError();}
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code;
}
inline DWORD run(const std::filesystem::path& root,const std::filesystem::path& output,
                 const std::wstring& log,const std::string& expected_hash) {
    return run_script(root,root/L"support"/L"collect-install-diagnostics.ps1",output,log,expected_hash);
}
}