#pragma once
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <filesystem>
#include <string>
#include "failure-collector.hpp"

namespace liukai::wix_diagnostics {
struct TempCollector {
    std::filesystem::path directory,script;
    HANDLE lock=INVALID_HANDLE_VALUE;
    ~TempCollector(){if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);if(!script.empty())DeleteFileW(script.c_str());if(!directory.empty())RemoveDirectoryW(directory.c_str());}
};
// The caller supplies the trusted Program Files directory in production.
// Extract just the pinned collector into a new directory and remove it after
// the child exits; no batch script or shell command is generated.
inline DWORD run_binary(MSIHANDLE database,const std::filesystem::path& staging_parent,
                        const std::filesystem::path& root,const std::filesystem::path& output,
                        const std::wstring& log,const std::string& expected_hash,
                        const std::wstring& stage=L"",const std::wstring& hresult=L"") {
    namespace files=boshiamy::installer::files;
    namespace durable=boshiamy::installer::durable_cli;
    if(!database||!files::detail::absolute_no_reparse(staging_parent,true))return ERROR_ACCESS_DENIED;
    std::string token;if(!durable::detail::make_token(&token))return ERROR_GEN_FAILURE;
    TempCollector temp;temp.directory=staging_parent/(L".LiuKai-diag-"+std::wstring(token.begin(),token.end()));
    if(!CreateDirectoryW(temp.directory.c_str(),nullptr)){temp.directory.clear();return GetLastError();}
    temp.script=temp.directory/L"collect-install-diagnostics.ps1";
    if(!files::detail::absolute_no_reparse(temp.directory,true))return ERROR_ACCESS_DENIED;
    MSIHANDLE view=0,record=0;UINT status=MsiDatabaseOpenViewW(database,L"SELECT `Data` FROM `Binary` WHERE `Name`='FailureDiagnostics'",&view);
    if(status==ERROR_SUCCESS)status=MsiViewExecute(view,0);
    if(status==ERROR_SUCCESS)status=MsiViewFetch(view,&record);
    if(status!=ERROR_SUCCESS){if(record)MsiCloseHandle(record);if(view)MsiCloseHandle(view);return status;}
    HANDLE out=CreateFileW(temp.script.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    if(out==INVALID_HANDLE_VALUE){const DWORD error=GetLastError();MsiCloseHandle(record);MsiCloseHandle(view);return error;}
    DWORD result=ERROR_SUCCESS,total=0;
    for(;;){char bytes[8192];DWORD size=sizeof(bytes);status=MsiRecordReadStream(record,1,bytes,&size);if(status!=ERROR_SUCCESS){result=status;break;}if(!size)break;
        if(total>1024*1024-size){result=ERROR_FILE_TOO_LARGE;break;}total+=size;DWORD written=0;
        if(!WriteFile(out,bytes,size,&written,nullptr)||written!=size){result=GetLastError();if(result==ERROR_SUCCESS)result=ERROR_WRITE_FAULT;break;}}
    if(!FlushFileBuffers(out)&&result==ERROR_SUCCESS)result=GetLastError();CloseHandle(out);MsiCloseHandle(record);MsiCloseHandle(view);
    if(result!=ERROR_SUCCESS||!total)return result!=ERROR_SUCCESS?result:ERROR_INVALID_DATA;
    temp.lock=CreateFileW(temp.script.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(temp.lock==INVALID_HANDLE_VALUE)return GetLastError();
    return run_script(root,temp.script,output,log,expected_hash,stage,hresult);
}
}