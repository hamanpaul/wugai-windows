#include "../installer/tsf_shared_store.hpp"
#include "../installer/durable_cli_session.hpp"
#include <iostream>
using namespace boshiamy::installer;
int main(){
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(init))return 2;
    wchar_t programFiles[32768]{};DWORD n=GetEnvironmentVariableW(L"ProgramFiles",programFiles,32768);
    if(!n||n>=32768){CoUninitialize();return 2;}
    const auto root=std::filesystem::path(programFiles)/L"LiuKai";
    HRESULT opened=E_FAIL,noProjection=E_FAIL,withProjection=E_FAIL;
    {
    WindowsTsfSharedApi api;RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,L"LiuKai-TSF-v1");
    WindowsTsfSharedStore shared(api,markers,L"LiuKai-TSF-v1");std::unique_ptr<WindowsInstallerBackend> backend;
    opened=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,L"LiuKai-TSF-v1",shared,&backend,true,root.wstring(),KEY_READ);
    if(opened==S_OK){
        noProjection=durable_cli::detail::inspect_machine_history(root,"LiuKai-TSF-v1");
        withProjection=durable_cli::detail::inspect_machine_history(root,"LiuKai-TSF-v1",[&backend](Resource r,const ResourceState& s,ResourceState* out){return backend->project_apply_state(r,s,out);});
    }
    }
    CoUninitialize();std::cout<<"open="<<static_cast<uint32_t>(opened)<<" without_projection="<<static_cast<uint32_t>(noProjection)<<" with_projection="<<static_cast<uint32_t>(withProjection)<<"\n";
    return opened==S_OK&&withProjection==S_OK?0:1;
}
