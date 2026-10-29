#pragma once
#ifdef _WIN32
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace boshiamy::installer::independent_categories {
inline const CLSID service = {0x5d7c4291,0x2b34,0x4ce8,{0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
inline constexpr wchar_t child_argument[] = L"--liukai-internal-enum-categories";
inline constexpr wchar_t caps_argument[] = L"--liukai-internal-profile-caps";
inline constexpr wchar_t profile_argument[] = L"--liukai-internal-profile-present";

inline HRESULT read_profile_present_local(bool* present) {
    if(!present)return E_POINTER;
    *present=false;
    const GUID profile={0x7254a21f,0x6eb1,0x4217,{0xa4,0x2c,0xc2,0x73,0x01,0x17,0x5d,0x90}};
    ITfInputProcessorProfileMgr* manager=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&manager));
    if(hr!=S_OK||!manager)return hr!=S_OK?hr:E_UNEXPECTED;
    IEnumTfInputProcessorProfiles* rows=nullptr;
    hr=manager->EnumProfiles(0x0404,&rows);
    if(hr!=S_OK||!rows){manager->Release();return hr!=S_OK?hr:E_UNEXPECTED;}
    for(;;) {
        TF_INPUTPROCESSORPROFILE row{};ULONG count=0;hr=rows->Next(1,&row,&count);
        if(hr==S_FALSE&&count==0){hr=S_OK;break;}
        if(hr!=S_OK||count!=1){if(hr==S_OK)hr=E_UNEXPECTED;break;}
        if(row.dwProfileType==TF_PROFILETYPE_INPUTPROCESSOR&&row.langid==0x0404&&
           IsEqualCLSID(row.clsid,service)&&IsEqualGUID(row.guidProfile,profile)){*present=true;break;}
    }
    rows->Release();manager->Release();return hr;
}
inline int profile_child_main() {
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return 2;
    bool present=false;HRESULT hr=read_profile_present_local(&present);
    const uint32_t data[]={static_cast<uint32_t>(hr),present?1u:0u};DWORD written=0;
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    const bool ok=output&&output!=INVALID_HANDLE_VALUE&&WriteFile(output,data,sizeof(data),&written,nullptr)&&written==sizeof(data);
    CoUninitialize();return ok?0:1;
}

// Called only in a fresh child process, before it has observed or modified a
// category. TSF may retain an older category catalog inside a writer process.
inline HRESULT read_local(std::vector<GUID>* out) {
    if(!out)return E_POINTER;
    out->clear();ITfCategoryMgr* manager=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfCategoryMgr,reinterpret_cast<void**>(&manager));
    if(hr!=S_OK||!manager)return hr!=S_OK?hr:E_UNEXPECTED;
    IEnumGUID* rows=nullptr;hr=manager->EnumCategoriesInItem(service,&rows);
    if(hr!=S_OK||!rows){manager->Release();return hr!=S_OK?hr:E_UNEXPECTED;}
    for(;;) {
        GUID id{};ULONG count=0;hr=rows->Next(1,&id,&count);
        if(hr==S_FALSE&&count==0){hr=S_OK;break;}
        if(hr!=S_OK||count!=1){if(hr==S_OK)hr=E_UNEXPECTED;break;}
        if(out->size()>=64){hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);break;}
        out->push_back(id);
    }
    rows->Release();manager->Release();return hr;
}
inline int child_main() {
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return 2;
    std::vector<GUID> rows;HRESULT hr=read_local(&rows);
    if(hr==S_OK) {
        const uint32_t count=static_cast<uint32_t>(rows.size());DWORD written=0;
        HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
        if(!output||output==INVALID_HANDLE_VALUE||!WriteFile(output,&count,sizeof(count),&written,nullptr)||written!=sizeof(count))hr=E_FAIL;
        const DWORD bytes=count*sizeof(GUID);
        if(hr==S_OK&&bytes&&(!WriteFile(output,rows.data(),bytes,&written,nullptr)||written!=bytes))hr=E_FAIL;
    }
    CoUninitialize();return hr==S_OK?0:1;
}

inline HRESULT read_profile_caps_local(DWORD* caps) {
    if(!caps)return E_POINTER;
    *caps=0;
    const GUID profile={0x7254a21f,0x6eb1,0x4217,{0xa4,0x2c,0xc2,0x73,0x01,0x17,0x5d,0x90}};
    ITfInputProcessorProfileMgr* manager=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&manager));
    if(hr!=S_OK||!manager)return hr!=S_OK?hr:E_UNEXPECTED;
    // Match the normal status sequence and verify profile identity first.
    IEnumTfInputProcessorProfiles* enumeration=nullptr;
    hr=manager->EnumProfiles(0x0404,&enumeration);
    if(hr!=S_OK||!enumeration){manager->Release();return hr!=S_OK?hr:E_UNEXPECTED;}
    bool present=false;
    for(;;) {
        TF_INPUTPROCESSORPROFILE row{};ULONG count=0;
        hr=enumeration->Next(1,&row,&count);
        if(hr==S_FALSE&&count==0){hr=S_OK;break;}
        if(hr!=S_OK||count!=1){if(hr==S_OK)hr=E_UNEXPECTED;break;}
        if(row.dwProfileType==TF_PROFILETYPE_INPUTPROCESSOR&&row.langid==0x0404&&
           IsEqualCLSID(row.clsid,service)&&IsEqualGUID(row.guidProfile,profile)) {present=true;break;}
    }
    enumeration->Release();
    if(hr!=S_OK||!present){manager->Release();return hr!=S_OK?hr:S_FALSE;}
    TF_INPUTPROCESSORPROFILE found{};
    hr=manager->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR,0x0404,service,profile,nullptr,&found);
    manager->Release();
    if(hr!=S_OK)return hr;
    if(found.dwProfileType!=TF_PROFILETYPE_INPUTPROCESSOR||found.langid!=0x0404||
       !IsEqualCLSID(found.clsid,service)||!IsEqualGUID(found.guidProfile,profile))return E_UNEXPECTED;
    *caps=found.dwCaps;return S_OK;
}
inline int caps_child_main() {
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return 2;
    DWORD caps=0;HRESULT hr=read_profile_caps_local(&caps);
    const uint32_t data[]={static_cast<uint32_t>(hr),caps};DWORD written=0;
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    const bool ok=output&&output!=INVALID_HANDLE_VALUE&&WriteFile(output,data,sizeof(data),&written,nullptr)&&written==sizeof(data);
    CoUninitialize();return ok?0:1;
}

// The pipe carries at most 1028 bytes. Fail closed on process creation,
// timeout, malformed output, or enumeration error; no cached state is adopted.
inline HRESULT read_child_data(const wchar_t* argument,std::vector<unsigned char>* output) {
    if(!argument||!output)return E_POINTER;
    output->clear();
    std::vector<wchar_t> exe(32768);
    DWORD length=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
    if(!length||length>=exe.size())return HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_INSUFFICIENT_BUFFER);
    std::wstring command=L"\""+std::wstring(exe.data(),length)+L"\" "+argument;
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE read_pipe=nullptr,write_pipe=nullptr;
    if(!CreatePipe(&read_pipe,&write_pipe,&security,4096))return HRESULT_FROM_WIN32(GetLastError());
    if(!SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0)) {
        DWORD error=GetLastError();CloseHandle(read_pipe);CloseHandle(write_pipe);return HRESULT_FROM_WIN32(error);
    }
    HANDLE null_io=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(null_io==INVALID_HANDLE_VALUE){DWORD error=GetLastError();CloseHandle(read_pipe);CloseHandle(write_pipe);return HRESULT_FROM_WIN32(error);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=null_io;startup.hStdOutput=write_pipe;startup.hStdError=null_io;
    PROCESS_INFORMATION child{};
    BOOL started=CreateProcessW(exe.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child);
    DWORD create_error=started?ERROR_SUCCESS:GetLastError();CloseHandle(write_pipe);CloseHandle(null_io);
    if(!started){CloseHandle(read_pipe);return HRESULT_FROM_WIN32(create_error);}
    const DWORD wait=WaitForSingleObject(child.hProcess,15000);
    if(wait!=WAIT_OBJECT_0) {
        DWORD error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
        TerminateProcess(child.hProcess,1);WaitForSingleObject(child.hProcess,1000);
        CloseHandle(child.hThread);CloseHandle(child.hProcess);CloseHandle(read_pipe);return HRESULT_FROM_WIN32(error);
    }
    DWORD exit_code=1;BOOL got_exit=GetExitCodeProcess(child.hProcess,&exit_code);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    if(!got_exit||exit_code!=0){
        std::fprintf(stderr,"Independent TSF query child failed: exit_code=%lu, exit_read=%u\n",
            static_cast<unsigned long>(exit_code),static_cast<unsigned>(got_exit));
        CloseHandle(read_pipe);
        if(got_exit&&exit_code>=0x80000000u)return static_cast<HRESULT>(exit_code);
        return got_exit&&exit_code?HRESULT_FROM_WIN32(exit_code):E_FAIL;
    }
    unsigned char bytes[sizeof(uint32_t)+64*sizeof(GUID)+1]{};DWORD total=0,read=0;
    while(total<sizeof(bytes) && ReadFile(read_pipe,bytes+total,static_cast<DWORD>(sizeof(bytes)-total),&read,nullptr)&&read)total+=read;
    CloseHandle(read_pipe);
    output->assign(bytes,bytes+total);return S_OK;
}
inline HRESULT read_fresh(std::vector<GUID>* out) {
    if(!out)return E_POINTER;
    out->clear();std::vector<unsigned char> bytes;
    const HRESULT hr=read_child_data(child_argument,&bytes);
    if(hr!=S_OK)return hr;
    if(bytes.size()<sizeof(uint32_t))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    uint32_t count=0;std::memcpy(&count,bytes.data(),sizeof(count));
    if(count>64||bytes.size()!=sizeof(count)+count*sizeof(GUID))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    out->resize(count);if(count)std::memcpy(out->data(),bytes.data()+sizeof(count),count*sizeof(GUID));
    return S_OK;
}
inline HRESULT read_profile_present_fresh(bool* present) {
    if(!present)return E_POINTER;
    *present=false;std::vector<unsigned char> bytes;
    const HRESULT hr=read_child_data(profile_argument,&bytes);
    if(hr!=S_OK)return hr;
    if(bytes.size()!=2*sizeof(uint32_t))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    uint32_t values[2]{};std::memcpy(values,bytes.data(),sizeof(values));
    const HRESULT observed=static_cast<HRESULT>(values[0]);
    if(observed!=S_OK)return observed;
    if(values[1]>1)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    *present=values[1]!=0;return S_OK;
}
template<class Query,class Delay>
inline HRESULT retry_presence(Query query,Delay delay,bool expected,unsigned attempts,bool* present) {
    if(!present||attempts==0)return E_INVALIDARG;
    *present=false;
    for(unsigned attempt=0;attempt<attempts;++attempt) {
        bool observed=false;const HRESULT status=query(&observed);
        if(status!=S_OK)return status;
        *present=observed;
        if(observed==expected)return S_OK;
        if(attempt+1<attempts)delay();
    }
    // A mismatched observation is not evidence that the mutation settled.
    return HRESULT_FROM_WIN32(ERROR_RETRY);
}
template<class Query,class Delay>
inline HRESULT retry_profile_caps(Query query,Delay delay,DWORD required,unsigned attempts,DWORD* caps) {
    if(!caps||attempts==0)return E_INVALIDARG;
    *caps=0;
    for(unsigned attempt=0;attempt<attempts;++attempt) {
        DWORD observed_caps=0;HRESULT observed=query(&observed_caps);
        if(observed==S_OK)*caps=observed_caps;
        const bool pending=observed==S_FALSE||(observed==S_OK&&required&&(observed_caps&required)!=required);
        if(pending&&attempt+1<attempts){delay();continue;}
        return observed;
    }
    return E_UNEXPECTED;
}
inline HRESULT read_profile_caps_fresh(DWORD* caps, bool retry_missing=false, DWORD required=0) {
    if(!caps)return E_POINTER;
    const auto query=[](DWORD* observed_caps)->HRESULT {
        std::vector<unsigned char> bytes;
        const HRESULT hr=read_child_data(caps_argument,&bytes);
        if(hr!=S_OK)return hr;
        if(bytes.size()!=2*sizeof(uint32_t))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        uint32_t values[2]{};std::memcpy(values,bytes.data(),sizeof(values));
        const HRESULT observed=static_cast<HRESULT>(values[0]);
        if(observed!=S_OK&&observed!=S_FALSE)
            std::fprintf(stderr,"Independent TSF profile capability API returned %lu\n",static_cast<unsigned long>(values[0]));
        if(observed==S_OK)*observed_caps=values[1];
        return observed;
    };
    return retry_profile_caps(query,[]{Sleep(250);},required,retry_missing?40u:1u,caps);
}
} // namespace boshiamy::installer::independent_categories
#endif
