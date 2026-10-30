#include "../installer/independent_categories.hpp"
#include <algorithm>
#include <iostream>
using namespace boshiamy::installer::independent_categories;
int wmain(int argc,wchar_t** argv) {
    if(argc==2 && std::wstring(argv[1])==child_argument)return child_main();
    if(argc==2 && std::wstring(argv[1])==caps_argument)return caps_child_main();
    if(argc==2 && std::wstring(argv[1])==profile_argument)return profile_child_main();
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return 1;
    std::vector<GUID> direct,fresh;
    HRESULT first=read_local(&direct),second=read_fresh(&fresh);
    bool same=first==S_OK&&second==S_OK&&direct.size()==fresh.size()&&
        std::all_of(direct.begin(),direct.end(),[&](const GUID& value){return std::any_of(fresh.begin(),fresh.end(),[&](const GUID& other){return IsEqualGUID(value,other);});});
    DWORD local_caps=0,fresh_caps=0;
    HRESULT local_result=read_profile_caps_local(&local_caps),fresh_result=read_profile_caps_fresh(&fresh_caps);
    same=same&&local_result==fresh_result&&(local_result!=S_OK||local_caps==fresh_caps);
    bool local_present=false,fresh_present=false;
    HRESULT local_presence=read_profile_present_local(&local_present),fresh_presence=read_profile_present_fresh(&fresh_present);
    same=same&&local_presence==S_OK&&fresh_presence==S_OK&&local_present==fresh_present;
    unsigned presence_calls=0,presence_delays=0;bool presence=false;
    auto delayed_presence=[&](bool* out)->HRESULT {*out=(++presence_calls>=3);return S_OK;};
    same=same&&retry_presence(delayed_presence,[&]{++presence_delays;},true,4,&presence)==S_OK&&
        presence&&presence_calls==3&&presence_delays==2;
    presence_calls=0;presence_delays=0;
    auto stale_presence=[&](bool* out)->HRESULT {++presence_calls;*out=true;return S_OK;};
    same=same&&retry_presence(stale_presence,[&]{++presence_delays;},false,3,&presence)==HRESULT_FROM_WIN32(ERROR_RETRY)&&
        presence&&presence_calls==3&&presence_delays==2;
    auto failed_presence=[](bool*)->HRESULT {return E_ACCESSDENIED;};
    same=same&&retry_presence(failed_presence,[]{},true,3,&presence)==E_ACCESSDENIED;
    // An existing profile can appear before its newly registered capability.
    // Keep polling until that exact bit arrives; do not invent success on timeout.
    unsigned calls=0,delays=0;DWORD caps=0;
    auto eventually=[&](DWORD* out)->HRESULT {++calls;if(calls==1)return S_FALSE;*out=calls==2?0x20000u:0x20004u;return S_OK;};
    same=same&&retry_profile_caps(eventually,[&]{++delays;},4,5,&caps)==S_OK&&calls==3&&delays==2&&caps==0x20004u;
    calls=0;delays=0;
    auto never=[&](DWORD* out)->HRESULT {++calls;*out=0x20000u;return S_OK;};
    same=same&&retry_profile_caps(never,[&]{++delays;},4,3,&caps)==S_OK&&calls==3&&delays==2&&caps==0x20000u;
    CoUninitialize();
    if(!same){std::cerr << "Independent read differs from a new direct manager\n";return 1;}
    std::cout << "independent category child-process read passed, categories=" << fresh.size() << "\n";
    return 0;
}
