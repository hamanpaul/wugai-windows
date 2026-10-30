#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <vector>
#include "../wix/retired-files.hpp"

static bool prop(MSIHANDLE h,const wchar_t* name,std::wstring& value) {
    wchar_t empty[1]{};DWORD n=0;UINT rc=MsiGetPropertyW(h,name,empty,&n);
    if(rc!=ERROR_MORE_DATA&&rc!=ERROR_SUCCESS)return false;
    std::vector<wchar_t> b(n+1);DWORD capacity=n+1;
    if(MsiGetPropertyW(h,name,b.data(),&capacity)!=ERROR_SUCCESS)return false;
    value.assign(b.data(),capacity);return true;
}
extern "C" __declspec(dllexport) UINT __stdcall Prepare(MSIHANDLE h) {
    std::wstring root,fail,skip;if(!prop(h,L"INSTALLDIR",root)||root.empty()||!prop(h,L"FAIL_COMMIT",fail)||!prop(h,L"SKIP_REBOOT_MODE",skip))return ERROR_INSTALL_FAILURE;
    const std::wstring data=root+L"|"+skip;
    if(fail.empty())fail=L"0";
    if(skip!=L"1"&&MsiSetMode(h,MSIRUNMODE_REBOOTATEND,TRUE)!=ERROR_SUCCESS)return ERROR_INSTALL_FAILURE;
    return MsiSetPropertyW(h,L"Undo",data.c_str())==ERROR_SUCCESS && MsiSetPropertyW(h,L"Retire",data.c_str())==ERROR_SUCCESS && MsiSetPropertyW(h,L"CommitRetire",fail.c_str())==ERROR_SUCCESS?ERROR_SUCCESS:ERROR_INSTALL_FAILURE;
}
static UINT run(MSIHANDLE h,bool undo) {
    std::wstring root;if(!prop(h,L"CustomActionData",root))return ERROR_INSTALL_FAILURE;
    const auto separator=root.rfind(L'|');if(separator==std::wstring::npos)return ERROR_INSTALL_FAILURE;
    const bool skip=root.substr(separator+1)==L"1";root.resize(separator);
    const auto dir=std::filesystem::path(root),original=dir/L"fixture.dll",retired=dir/L"retired.dll";
    const auto hash=liukai::retired_files::sha256_file(undo?retired:original);
    if(hash.empty())return ERROR_INSTALL_FAILURE;
    const HRESULT result=undo?liukai::retired_files::restore(original,retired,hash):liukai::retired_files::retire(original,retired,hash);
    if(result!=S_OK)return ERROR_INSTALL_FAILURE;
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) UINT __stdcall Retire(MSIHANDLE h) {return run(h,false);}
extern "C" __declspec(dllexport) UINT __stdcall Undo(MSIHANDLE h) {return run(h,true);}
extern "C" __declspec(dllexport) UINT __stdcall ForceFailure(MSIHANDLE) {return ERROR_INSTALL_FAILURE;}
extern "C" __declspec(dllexport) UINT __stdcall CommitRetire(MSIHANDLE h) {
    std::wstring fail;if(!prop(h,L"CustomActionData",fail))return ERROR_INSTALL_FAILURE;
    if(fail==L"1")return ERROR_INSTALL_FAILURE;
    return ERROR_SUCCESS;
}
